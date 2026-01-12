import json
import os
import logging
import time
import asyncio
import aiohttp
from typing import Dict, Optional, Union

class SecHttpClient:
    """
    Manages asynchronous HTTP GET requests specifically tailored for SEC EDGAR endpoints.

    This class handles crucial aspects of interacting with the SEC API, including:
    - Maintaining a persistent `aiohttp.ClientSession` for connection pooling.
    - Enforcing rate limiting based on SEC guidelines (default 10 requests/sec).
    - Automatically retrying requests on transient errors (like 429 Rate Limit Exceeded)
      with exponential backoff.
    - Handling standard HTTP errors and logging appropriately.
    - Managing the required `User-Agent` header for all requests.
    - Providing a standardized interface (`make_request`) for fetching data (JSON or text).

    It's designed to be instantiated by the main `SECDataFetcher` and used by other
    components (like `FilingDocumentHandler`) that need to make direct HTTP calls.
    """

    def __init__(self, user_agent: str, rate_limit_sleep: float = 0.1):
        """
        Initializes the SEC-specific asynchronous HTTP client.

        Args:
            user_agent (str): The mandatory User-Agent string required by the SEC API.
                Format: "Sample Company Name AdminContact@example.com". This value
                should uniquely identify your application or organization. If None or
                empty, warnings will be logged, and requests may fail.
            rate_limit_sleep (float, optional): The minimum time interval (in seconds)
                to wait between consecutive requests to avoid hitting SEC rate limits.
                Defaults to 0.1 seconds (10 requests per second).
        """
        self.user_agent = self._normalize_user_agent(user_agent)
        if self.user_agent:
            logging.info(f"SecHttpClient using User-Agent: {self.user_agent}")
        else:
            logging.warning("SecHttpClient initialized without a User-Agent. SEC requests may fail.")
            logging.warning("Provide via constructor or set SEC_API_USER_AGENT env var.")
            logging.warning("Format: 'Name (first/last) your@email.com'")

        self.default_headers = {
            "User-Agent": self.user_agent or "Default Agent (email@example.com)", # Fallback needed
            "Accept-Encoding": "gzip, deflate",
            "Host": "data.sec.gov" # Default host, may need overrides
        }
        self.request_interval = rate_limit_sleep
        self.last_request_time = 0
        self._session: Optional[aiohttp.ClientSession] = None
        self._archive_session: Optional[aiohttp.ClientSession] = None

    def _normalize_user_agent(self, user_agent: str) -> str:
        """
        Normalizes the User-Agent string to ensure strict SEC compliance.
        
        If the user_agent is in format "Name Email", converts it to "Name/1.0 (Email)".
        If already properly formatted, returns as-is.
        
        Args:
            user_agent (str): The original user agent string
            
        Returns:
            str: The normalized user agent string
        """
        if not user_agent:
            return user_agent
            
        # Check if it's in "Name Email" format without version/parentheses
        parts = user_agent.strip().split()
        if len(parts) >= 2:
            # Look for email pattern (contains @)
            email_part = None
            name_parts = []
            
            for part in parts:
                if '@' in part:
                    email_part = part
                else:
                    name_parts.append(part)
            
            # If we found an email and it's not already in proper format
            if email_part and '(' not in user_agent and '/' not in user_agent:
                name = ' '.join(name_parts)
                normalized = f"{name}/1.0 ({email_part})"
                logging.info(f"Normalized User-Agent from '{user_agent}' to '{normalized}'")
                return normalized
        
        return user_agent

    async def _get_session(self) -> aiohttp.ClientSession:
        """
        Lazily initializes and returns the shared `aiohttp.ClientSession`.

        Creates a new session if one doesn't exist or if the existing one is closed.
        This promotes connection reuse, which is more efficient.

        Returns:
            aiohttp.ClientSession: The active client session.
        """
        if self._session is None or self._session.closed:
            logging.debug("Initializing new aiohttp ClientSession.")
            self._session = aiohttp.ClientSession()
        return self._session

    async def _get_archive_session(self) -> aiohttp.ClientSession:
        """
        Lazily initializes and returns a separate ClientSession specifically for Archives.
        
        This ensures clean separation between data.sec.gov and www.sec.gov requests
        to avoid connection pool contamination.

        Returns:
            aiohttp.ClientSession: The active archive client session.
        """
        if self._archive_session is None or self._archive_session.closed:
            logging.debug("Initializing new aiohttp ClientSession for Archives.")
            self._archive_session = aiohttp.ClientSession()
        return self._archive_session

    async def make_request(self, url: str, max_retries: int = 3, headers: Optional[Dict] = None, is_json: bool = True) -> Optional[Union[Dict, str]]:
        """
        Performs a rate-limited, retrying asynchronous HTTP GET request to a given URL.

        This is the primary method for fetching data from SEC endpoints. It incorporates
        rate limiting delays, handles standard HTTP errors (4xx/5xx), retries on specific
        conditions (like 429 Too Many Requests), and parses the response.

        Args:
            url (str): The full URL to fetch data from.
            max_retries (int, optional): The maximum number of times to retry the request
                if it fails due to rate limiting or other transient network issues.
                Defaults to 3.
            headers (Optional[Dict], optional): A dictionary of custom HTTP headers to use
                for this specific request. If provided, these headers *replace* the
                default headers (including User-Agent). It's crucial to include a valid
                'User-Agent' in custom headers if used. If None, the client's default
                headers (including the initialized User-Agent) are used. Defaults to None.
            is_json (bool, optional): Determines how the response content is processed.
                If True, attempts to parse the response body as JSON and returns a dict.
                If False, returns the raw response body as a string. If JSON parsing
                is requested but fails, the raw text is returned with a warning log.
                Defaults to True.

        Returns:
            Optional[Union[Dict, str]]: The parsed JSON data as a dictionary (if `is_json`
                is True and parsing succeeds), the raw response text as a string (if
                `is_json` is False, or if JSON parsing fails), or None if the request
                ultimately fails after all retries or encounters a non-retryable error
                (e.g., 404 Not Found).
        """
        current_time = time.time()
        time_since_last = current_time - self.last_request_time

        if time_since_last < self.request_interval:
            sleep_time = self.request_interval - time_since_last
            logging.debug(f"Rate limit: sleeping for {sleep_time:.3f}s")
            await asyncio.sleep(sleep_time)

        session = await self._get_session()

        # Determine which headers to use - prioritize provided headers, fallback to default
        request_headers = headers if headers is not None else self.default_headers
        # Ensure User-Agent is present, warn if missing entirely
        if not request_headers.get('User-Agent'):
             logging.warning(f"User-Agent not found in request headers for {url}. Using default if possible.")
             # Fallback explicitly if custom headers were provided but lacked User-Agent
             if headers is not None and not headers.get('User-Agent') and self.default_headers.get('User-Agent'):
                   request_headers = self.default_headers
             elif not self.default_headers.get('User-Agent'):
                  logging.error(f"CRITICAL: No User-Agent available in default or custom headers for {url}. Request likely to fail.")
                  # Assign empty dict if absolutely no User-Agent is set anywhere, though request will likely fail
                  request_headers = request_headers or {}

        for attempt in range(max_retries):
            try:
                self.last_request_time = time.time()
                logging.debug(f"Making request (Attempt {attempt+1}/{max_retries}): GET {url} Headers: {request_headers}")
                # Use the determined headers
                async with session.get(url, headers=request_headers, timeout=10) as response:
                    logging.debug(f"Response status for {url}: {response.status}")
                    # Handle rate limiting (429)
                    if response.status == 429:
                        # Exponential backoff: 1, 3, 7 seconds (approx)
                        wait_time = (2 ** attempt) + float(response.headers.get('Retry-After', 1)) # Use Retry-After if available
                        wait_time = min(wait_time, 10) # Cap wait time
                        logging.warning(f"Rate limited (429) by SEC API. Waiting {wait_time:.2f}s before retry {attempt+1}/{max_retries} for {url}")
                        await asyncio.sleep(wait_time)
                        continue # Retry the loop

                    # Raise exception for other 4xx/5xx status codes
                    response.raise_for_status()

                    # Process successful response
                    if is_json:
                        text_content = await response.text()
                        try:
                            # Check if it looks like JSON before attempting to parse
                            if text_content.strip().startswith(('{', '[')):
                                return json.loads(text_content)
                            else:
                                logging.warning(f"Content from {url} doesn't appear to be JSON (starts with: {text_content[:50]}...). Returning as text.")
                                return text_content # Return text if not JSON-like
                        except json.JSONDecodeError as json_err:
                            logging.error(f"JSON decode error for {url}: {json_err}. Content length: {len(text_content)}.")
                            logging.warning(f"Returning raw text instead (first 100 chars): {text_content[:100]}...")
                            return text_content # Return raw text on decode error
                    else:
                        # Return raw text if JSON parsing wasn't requested
                        return await response.text()

            except asyncio.TimeoutError:
                 logging.warning(f"Request timeout on attempt {attempt+1}/{max_retries} for {url}")
            except aiohttp.ClientResponseError as e:
                 # Log specific HTTP errors
                 logging.error(f"HTTP error {e.status} on attempt {attempt+1}/{max_retries} for {url}: {e.message}")
                 # Don't retry on certain client errors like 404 Not Found
                 if e.status in [404, 400, 403]:
                     logging.error(f"Non-retryable client error {e.status} encountered. Aborting request for {url}.")
                     return None
            except aiohttp.ClientError as e:
                 # Catch other potential client errors (connection issues, etc.)
                 logging.warning(f"Client request error on attempt {attempt+1}/{max_retries} for {url}: {str(e)}")
            except Exception as e:
                 # Catch any other unexpected errors during the request
                 logging.error(f"Unexpected error during request attempt {attempt+1}/{max_retries} for {url}: {e}", exc_info=True)

            # Wait before the next retry (if not the last attempt)
            if attempt < max_retries - 1:
                retry_wait = 1 * (attempt + 1) # Simple linear backoff for general errors
                logging.debug(f"Waiting {retry_wait}s before next retry for {url}")
                await asyncio.sleep(retry_wait)

        logging.error(f"Request failed for {url} after {max_retries} retries.")
        return None

    async def make_archive_request(self, url: str, max_retries: int = 3, is_json: bool = False) -> Optional[Union[Dict, str]]:
        """
        Performs a rate-limited request specifically for SEC Archives endpoints.
        
        Uses specialized headers and separate session to avoid bot detection.
        Optimized for www.sec.gov/Archives/ endpoints.

        Args:
            url (str): The full URL to fetch from Archives
            max_retries (int): Maximum retry attempts  
            is_json (bool): Whether to parse as JSON (usually False for HTML/XML documents)

        Returns:
            Optional[Union[Dict, str]]: The response content or None if failed
        """
        # Rate limiting
        current_time = time.time()
        time_since_last = current_time - self.last_request_time

        if time_since_last < self.request_interval:
            sleep_time = self.request_interval - time_since_last
            logging.debug(f"Rate limit: sleeping for {sleep_time:.3f}s")
            await asyncio.sleep(sleep_time)

        session = await self._get_archive_session()

        # Archive-specific headers to avoid bot detection
        archive_headers = {
            "User-Agent": self.user_agent or "Default Agent (email@example.com)",
            "Accept": "text/html,application/xhtml+xml,application/xml;q=0.9,image/webp,*/*;q=0.8",
            "Accept-Encoding": "gzip, deflate, br",
            "Host": "www.sec.gov",
            "Connection": "keep-alive",
            "Upgrade-Insecure-Requests": "1",
            "Sec-Fetch-Dest": "document",
            "Sec-Fetch-Mode": "navigate",
            "Sec-Fetch-Site": "none"
        }

        for attempt in range(max_retries):
            try:
                self.last_request_time = time.time()
                logging.debug(f"Making archive request (Attempt {attempt+1}/{max_retries}): GET {url}")
                
                async with session.get(url, headers=archive_headers, timeout=15) as response:
                    logging.debug(f"Archive response status for {url}: {response.status}")
                    
                    # Handle rate limiting
                    if response.status == 429:
                        wait_time = (2 ** attempt) + float(response.headers.get('Retry-After', 1))
                        wait_time = min(wait_time, 15)
                        logging.warning(f"Archive rate limited (429). Waiting {wait_time:.2f}s before retry {attempt+1}/{max_retries}")
                        await asyncio.sleep(wait_time)
                        continue

                    # Raise exception for other errors
                    response.raise_for_status()

                    # Process successful response
                    if is_json:
                        text_content = await response.text()
                        try:
                            if text_content.strip().startswith(('{', '[')):
                                return json.loads(text_content)
                            else:
                                logging.warning(f"Archive content from {url} doesn't appear to be JSON. Returning as text.")
                                return text_content
                        except json.JSONDecodeError as json_err:
                            logging.error(f"JSON decode error for archive {url}: {json_err}")
                            return text_content
                    else:
                        return await response.text()

            except asyncio.TimeoutError:
                logging.warning(f"Archive request timeout on attempt {attempt+1}/{max_retries} for {url}")
            except aiohttp.ClientResponseError as e:
                logging.error(f"Archive HTTP error {e.status} on attempt {attempt+1}/{max_retries} for {url}: {e.message}")
                if e.status in [404, 400, 403]:
                    logging.error(f"Non-retryable archive error {e.status} encountered. Aborting request for {url}.")
                    return None
            except aiohttp.ClientError as e:
                logging.warning(f"Archive client error on attempt {attempt+1}/{max_retries} for {url}: {str(e)}")
            except Exception as e:
                logging.error(f"Unexpected archive error on attempt {attempt+1}/{max_retries} for {url}: {e}", exc_info=True)

            # Wait before retry
            if attempt < max_retries - 1:
                retry_wait = 1 * (attempt + 1)
                logging.debug(f"Waiting {retry_wait}s before next archive retry for {url}")
                await asyncio.sleep(retry_wait)

        logging.error(f"Archive request failed for {url} after {max_retries} retries.")
        return None

    async def test_api_access(self, test_endpoint_url: str) -> bool:
        """
        Performs a quick check to see if a specific SEC endpoint is reachable.

        This makes a single GET request to the provided URL using `make_request`
        with minimal retries. It's useful for verifying the User-Agent and network
        connectivity before performing more extensive operations.

        Args:
            test_endpoint_url (str): The full URL of an SEC API endpoint to test
                (e.g., `https://data.sec.gov/submissions/CIK0000320193.json`).

        Returns:
            bool: True if the request receives a successful response (implies the
                  endpoint is reachable and the User-Agent is likely accepted),
                  False otherwise (e.g., due to network errors, timeouts, or
                  non-retryable client/server errors like 403 Forbidden).
        """
        if not self.user_agent:
            logging.error("Cannot test API access: No User-Agent provided/configured.")
            return False

        logging.info(f"Testing SEC API connection to: {test_endpoint_url}")
        try:
            # Use make_request to test, expecting JSON but content doesn't matter
            response_data = await self.make_request(test_endpoint_url, max_retries=1, is_json=True)

            if response_data is not None:
                # Check if response wasn't an error string returned by make_request
                # A successful JSON parse (dict) or even non-JSON text means connection worked.
                logging.info(f"SEC API access test successful for {test_endpoint_url}")
                return True
            else:
                # make_request returned None, indicating failure after retries or non-retryable error
                logging.error(f"SEC API access test failed for {test_endpoint_url}: Request unsuccessful after retries.")
                return False

        except Exception as e:
            # Catch any unexpected errors during the test
            logging.error(f"Error testing SEC API access to {test_endpoint_url}: {str(e)}", exc_info=True)
            return False

    async def close(self):
        """
        Gracefully closes the underlying `aiohttp.ClientSession` instances.

        This should be called when the application using the `SecHttpClient` is shutting down
        to release network resources properly. It prevents potential resource leak warnings.
        Idempotent - safe to call multiple times or on already closed sessions.
        """
        if self._session and not self._session.closed:
            await self._session.close()
            logging.info("SecHttpClient aiohttp session closed.")
        self._session = None
        
        if self._archive_session and not self._archive_session.closed:
            await self._archive_session.close()
            logging.info("SecHttpClient archive aiohttp session closed.")
        self._archive_session = None 