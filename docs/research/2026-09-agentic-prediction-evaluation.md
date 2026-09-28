# Evaluating agent market predictions: literature scan (2026-09)

Research by Lt. Astra (Codex `gpt-6-astra`, web search), reviewed by the orchestrator.
Citation spot-check on 2026-09-27: references 3 and 5 confirmed on arXiv (title, authors; one
missing author added to 5); reference 2's DOI resolves to the publisher (page not readable by
the checker); the others were not individually re-checked. Treat numbers as the authors' claims.

Sentinel note: Sentinel persists the trade tape and 1-minute heatmap columns, but not raw L2
book deltas. Recommendation 4 (retain raw book updates with exchange and receipt timestamps)
therefore implies a new recorder before within-minute replay is possible.

## Executive summary

- **Blind replay is useful, but cannot certify absence of memorization.** Pair anonymization with genuinely forward forecasts or chronologically trained models.
- **Faster resolution does not automatically mean faster statistical power.** Overlapping outcomes and shared market regimes reduce effective sample size.
- **Order-book prediction is a demanding baseline:** published successes often concern individual price changes, much shorter than Sentinel’s minute columns.
- **Score probabilities separately from trading profitability.** Execution costs, latency, and uncertain fills can erase predictive advantages.
- **Automate discovery, restrict confirmation.** An autoresearch loop needs an immutable evaluator, complete trial history, and fresh confirmation data.

## 1. LLM financial forecasting: contamination and mitigation

Historical evaluation has two distinct leakage channels: future information entering the supplied features, and future information already embedded in model weights. Restricting retrieval to historical dates addresses only the first.

Glasserman and Lin find that anonymizing company identifiers can improve headline-based forecasting, including through reduced distraction from company knowledge; therefore, anonymization effects cannot be interpreted solely as evidence of memorization. Levy documents look-ahead bias in financial applications. [1](https://arxiv.org/abs/2309.17322), [2](https://doi.org/10.1111/1475-679x.70058)

Removing tickers/dates and expressing prices as relative returns reduces identification cues, but distinctive trajectories or news can remain recognizable. **Assessment:** normalization is a mitigation, not a contamination certificate.

Post-cutoff windows are stronger only when the cutoff covers the complete model pipeline, including subsequent tuning. Closed-provider contamination status is **UNVERIFIED** without an auditable training history. Stronger alternatives are predictions committed before outcomes exist, or time-sliced models: Kelly et al. construct monthly checkpoints from chronologically filtered text. [3](https://arxiv.org/abs/2607.11889)

## 2. Agentic systems and benchmarks

**TradingAgents** coordinates specialist analysts, bull/bear researchers, traders, and risk agents. Validation uses historical simulations and return, Sharpe, and drawdown comparisons. Its historical input restrictions do not themselves establish that pretrained models lacked future knowledge; independent live replication is **UNVERIFIED** here. [4](https://arxiv.org/abs/2412.20138)

**StockBench** evaluates sequential daily portfolio decisions using prices, fundamentals, and news, measuring cumulative return, drawdown, and Sortino ratio. Most evaluated agents struggle against buy-and-hold; rankings also depend on the evaluation window. Its “contamination-free” designation is an author claim, not an independently audited training-data guarantee. [5](https://arxiv.org/abs/2510.02209)

**ForecastBench** is the closer template for CopeNet’s ledger: forecasts concern unresolved future events, are scored after resolution, and are compared with human forecasts. Its original evaluation uses Brier scores. It validates probability forecasting, not executable trading alpha. [6](https://arxiv.org/abs/2409.19839)

Together these studies caution against equating financial knowledge, persuasive agent debate, or general reasoning scores with forecasting skill.

## 3. Order-book baselines, horizons, and realism

- **Classical:** Gould and Bonart fit logistic regression to bid/ask queue imbalance for the direction of the **next mid-price change**. This supplies a cheap probabilistic benchmark. [7](https://arxiv.org/abs/1512.03492)
- **Deep learning:** DeepLOB combines convolutions and LSTMs. Its LSE experiments include **20, 50, and 100 book-update horizons**, with thresholded up/flat/down labels derived from smoothed prices. Validation includes later periods and unseen instruments. Its trading simulation uses mid-prices **without transaction costs**. [8](https://arxiv.org/abs/1808.03668)
- **Representation:** Kolm, Turiel, and Westray find order-flow inputs outperform many raw-book models; their estimated effective stock-specific horizon is approximately **two average price changes**. That result is not evidence of hour-ahead BTC predictability. [9](https://doi.org/10.1111/mafi.12413)

For Sentinel, minute aggregation creates a different task. Recommended evaluation must include chronological regime changes, inference delay, fees, spread, slippage, and conservative fill assumptions. If only minute aggregates survive, exact within-minute event ordering and passive queue position cannot be reconstructed.

## 4. Scoring many small forecasts

Brier loss and logarithmic loss are strictly proper: expected performance rewards truthful probabilities. CRPS extends proper scoring to predictive distributions. Accuracy alone discards confidence and can reward majority-class guessing. [10](https://sites.stat.washington.edu/raftery/Research/PDF/Gneiting2007jasa.pdf)

Use paired score differences against baselines on identical cases. For uncertainty, preserve temporal dependence through block resampling or autocorrelation-robust inference; do not treat overlapping forecasts as independent observations.

For strategy search, the **deflated Sharpe ratio** adjusts for selection and non-normal returns; **probability of backtest overfitting** assesses how selected winners deteriorate across splits. Neither repairs contaminated features or guarantees future performance. Record unsuccessful trials too. [11](https://www.davidhbailey.com/dhbpapers/deflated-sharpe.pdf), [12](https://carmamaths.org/jon/backtest2.pdf)

Use chronological walk-forward evaluation. Purge training labels that overlap evaluation intervals; embargo nearby observations where dependence creates leakage. Fit normalization and calibration exclusively on permitted training data. [13](https://uat.store.wiley.com/en-us/advances-in-financial-machine-learning-p-9781119482109)

## 5. Self-improving research loops

Karpathy’s **autoresearch** supplies a concrete engineering pattern: constrained edits, fixed-budget experiments, a fixed evaluator, and keep/discard decisions. It demonstrates a research workflow, not financial profitability. [14](https://github.com/karpathy/autoresearch)

Repeated feedback makes an ostensibly held-out set part of the optimization process. Dwork et al. formalize this adaptive-analysis problem and develop controlled holdout reuse. Simply hiding individual labels while returning unlimited scores does not solve it. [15](https://pubmed.ncbi.nlm.nih.gov/26250683/)

For CopeNet, preregister each confirmation experiment’s hypothesis, candidate, baseline, target, metric, sample requirement, and stopping rule. Permit unrestricted experimentation only on development data.

## Design recommendations for the Sentinel+CopeNet ledger

These are proposed operating choices, not established profitability claims.

1. **Targets and horizons.** Make 15-minute mid-price direction the primary endpoint: \(P(m_{t+15}>m_t)\), with ties counted as false. Add 5- and 60-minute endpoints as exploratory. Secondary targets: upper/lower/no-barrier-hit probabilities and return quantiles. Fix barrier definitions using past-only volatility. Use scheduled cases rather than retrospectively selected “interesting” episodes.

2. **Baselines.** Include 0.5, rolling historical event frequency, momentum/reversal, regularized logistic regression, and gradient-boosted trees. Inputs: depth imbalance, spread, signed trade imbalance, lagged returns, and volatility. Add a compact temporal neural model later. Compare LLMs against the strongest baseline selected before confirmation, using identical information.

3. **Scoring.** Primary: mean Brier loss and paired improvement  
   \[
   d_i=(p_{\mathrm{baseline},i}-y_i)^2-(p_{\mathrm{agent},i}-y_i)^2.
   \]
   Report calibration, coverage, and day-block confidence intervals. Require forecasts for every scheduled case; preregister timeout handling. Maintain a separate execution ledger with fees, spread, slippage, inference expense, and measured delay.

4. **Blinding and timing.** Replace dates with relative time, remove asset/exchange identifiers, anchor prices to the current mid, and scale volumes using past-only statistics. Hide filenames and metadata; disable external retrieval. Use isolated sessions and immutable input hashes. Include only completed columns; prevent chart scaling from seeing future data. Start outcomes after forecast availability, reproducing agent delay in replay. Retain raw book updates and exchange/receipt timestamps.

5. **Holdout policy.** Partition chronologically into development, selection, and inaccessible confirmation periods; purge overlapping labels. Agents may inspect development and selection results. Freeze one challenger before opening confirmation. Once results influence changes, retire that block into development and use fresh data. Preregister a testing budget across promotions; never rewrite earlier ledger entries.

6. **Minimum samples.** No universal count guarantees power. Use a 30-day pilot to estimate dependence and paired-score variance. Proposed confirmation floor: **60 calendar days and 1,000 effective observations**, extended when power calculations require more. Illustratively, detecting Brier improvement \(\delta=0.01\), with paired-score standard deviation \(s_d=0.10\), 80% power, and two-sided 5% significance requires approximately  
   \[
   n_{\mathrm{eff}}\approx(2.80s_d/\delta)^2=784.
   \]
   This is an assumption-based calculation; dependence and multiplicity increase the required raw sample. Freeze the requirement before confirmation.

## References

1. Paul Glasserman and Caden Lin. **“Assessing Look-Ahead Bias in Stock Return Predictions Generated By GPT Sentiment Analysis.”** 2023, arXiv. [URL](https://arxiv.org/abs/2309.17322)
2. Bradford Levy. **“Caution Ahead: Numerical Reasoning and Look-Ahead Bias in AI Models.”** 2026, *Journal of Accounting Research*. [URL](https://doi.org/10.1111/1475-679x.70058)
3. Bryan Kelly, Semyon Malamud, Johannes Schwab, and Teng Andrea Xu. **“Scaling Point-in-Time Language Models.”** 2026, arXiv. [URL](https://arxiv.org/abs/2607.11889)
4. Yijia Xiao, Edward Sun, Di Luo, and Wei Wang. **“TradingAgents: Multi-Agents LLM Financial Trading Framework.”** 2024, arXiv; revised 2025. [URL](https://arxiv.org/abs/2412.20138)
5. Yanxu Chen, Zijun Yao, Yantao Liu, Amy Xin, Jin Ye, Jianing Yu, Lei Hou, and Juanzi Li. **“StockBench: Can LLM Agents Trade Stocks Profitably In Real-world Markets?”** 2025, arXiv. [URL](https://arxiv.org/abs/2510.02209)
6. Ezra Karger, Houtan Bastani, Chen Yueh-Han, Zachary Jacobs, Danny Halawi, Fred Zhang, and Philip E. Tetlock. **“ForecastBench: A Dynamic Benchmark of AI Forecasting Capabilities.”** 2024, arXiv; ICLR 2025. [URL](https://arxiv.org/abs/2409.19839)
7. Martin D. Gould and Julius Bonart. **“Queue Imbalance as a One-Tick-Ahead Price Predictor in a Limit Order Book.”** 2015, arXiv. [URL](https://arxiv.org/abs/1512.03492)
8. Zihao Zhang, Stefan Zohren, and Stephen Roberts. **“DeepLOB: Deep Convolutional Neural Networks for Limit Order Books.”** 2019, *IEEE Transactions on Signal Processing*. [URL](https://arxiv.org/abs/1808.03668)
9. Petter N. Kolm, Jeremy Turiel, and Nicholas Westray. **“Deep Order Flow Imbalance: Extracting Alpha at Multiple Horizons from the Limit Order Book.”** 2023, *Mathematical Finance*. [URL](https://doi.org/10.1111/mafi.12413)
10. Tilmann Gneiting and Adrian E. Raftery. **“Strictly Proper Scoring Rules, Prediction, and Estimation.”** 2007, *JASA*. [URL](https://sites.stat.washington.edu/raftery/Research/PDF/Gneiting2007jasa.pdf)
11. David H. Bailey and Marcos López de Prado. **“The Deflated Sharpe Ratio: Correcting for Selection Bias, Backtest Overfitting and Non-Normality.”** 2014, *Journal of Portfolio Management*. [URL](https://www.davidhbailey.com/dhbpapers/deflated-sharpe.pdf)
12. David H. Bailey, Jonathan M. Borwein, Marcos López de Prado, and Qiji Jim Zhu. **“The Probability of Backtest Overfitting.”** 2017, *Journal of Computational Finance*; linked 2014 manuscript. [URL](https://carmamaths.org/jon/backtest2.pdf)
13. Marcos López de Prado. **“Advances in Financial Machine Learning.”** 2018, Wiley, chapters 7 and 12. [URL](https://uat.store.wiley.com/en-us/advances-in-financial-machine-learning-p-9781119482109)
14. Andrej Karpathy. **“autoresearch.”** 2026, GitHub repository and experiment instructions. [URL](https://github.com/karpathy/autoresearch)
15. Cynthia Dwork, Vitaly Feldman, Moritz Hardt, Toniann Pitassi, Omer Reingold, and Aaron Roth. **“The Reusable Holdout: Preserving Validity in Adaptive Data Analysis.”** 2015, *Science*. [URL](https://pubmed.ncbi.nlm.nih.gov/26250683/)