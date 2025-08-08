#!/usr/bin/env python3
"""
Generate Test Order Book Data for Fractal Zoom Testing

Creates 1 hour of realistic BTC order book data at 100ms intervals.
This allows immediate testing of fractal zoom aggregation across all timeframes:
100ms → 500ms → 1sec → 1min → 5min → 15min → 60min

Usage:
    python3 scripts/generate_test_orderbook.py
    
Output:
    logs/test_orderbook_data.json - 1 hour of order book snapshots
"""

import json
import random
import time
from datetime import datetime, timedelta
from typing import List, Dict, Tuple

class OrderBookGenerator:
    def __init__(self, base_price: float = 108000.0, base_spread: float = 0.01):
        self.base_price = base_price
        self.base_spread = base_spread
        self.current_price = base_price
        self.price_trend = 0.0  # Price momentum
        
    def generate_price_movement(self) -> float:
        """Generate realistic price movement with trend and volatility"""
        # Random walk with momentum
        volatility = 0.0001  # 0.01% per 100ms
        trend_strength = 0.00005  # Trend component
        
        # Add some trend momentum
        trend_change = random.gauss(0, 0.0001)
        self.price_trend = self.price_trend * 0.99 + trend_change  # Trend decay
        
        # Calculate price change
        random_component = random.gauss(0, volatility)
        price_change = self.price_trend + random_component
        
        # Update price
        self.current_price *= (1 + price_change)
        
        # Keep price in reasonable range
        self.current_price = max(100000, min(120000, self.current_price))
        
        return self.current_price
    
    def generate_order_levels(self, mid_price: float, side: str, num_levels: int = 50) -> List[Dict]:
        """Generate realistic order book levels"""
        levels = []
        
        # Spread starts at base spread and widens with distance
        spread_multiplier = 1.0 if side == 'bid' else -1.0
        base_offset = self.base_spread * spread_multiplier
        
        for i in range(num_levels):
            # Price levels get wider as we go deeper
            level_offset = base_offset * (1 + i * 0.1)
            price = mid_price + (mid_price * level_offset)
            
            # Volume decreases with distance from mid, but has some randomness
            base_volume = 10.0 * (1 / (1 + i * 0.2))  # Decreasing volume
            volume_noise = random.uniform(0.5, 2.0)    # Random variation
            volume = base_volume * volume_noise
            
            # Add some large orders occasionally
            if random.random() < 0.05:  # 5% chance of large order
                volume *= random.uniform(5, 20)
            
            levels.append({
                'price': round(price, 2),
                'size': round(volume, 6)
            })
        
        # Sort appropriately
        if side == 'bid':
            levels.sort(key=lambda x: x['price'], reverse=True)
        else:
            levels.sort(key=lambda x: x['price'])
            
        return levels
    
    def generate_snapshot(self, timestamp_ms: int) -> Dict:
        """Generate a single order book snapshot"""
        current_price = self.generate_price_movement()
        
        # Generate bid and ask levels
        bids = self.generate_order_levels(current_price, 'bid', 50)
        asks = self.generate_order_levels(current_price, 'ask', 50)
        
        return {
            'timestamp': timestamp_ms,
            'symbol': 'BTC-USD',
            'bids': bids,
            'asks': asks,
            'mid_price': current_price,
            'spread': asks[0]['price'] - bids[0]['price'] if bids and asks else 0.0
        }

def generate_test_data(duration_hours: int = 1, interval_ms: int = 100) -> List[Dict]:
    """Generate test order book data for specified duration"""
    print(f"🚀 Generating {duration_hours} hour(s) of test order book data...")
    print(f"   Interval: {interval_ms}ms")
    
    # Calculate number of snapshots
    total_ms = duration_hours * 60 * 60 * 1000
    num_snapshots = total_ms // interval_ms
    
    print(f"   Total snapshots: {num_snapshots:,}")
    print(f"   Estimated file size: ~{num_snapshots * 2 / 1000:.1f} KB")
    
    # Generate data
    generator = OrderBookGenerator()
    snapshots = []
    
    # Start from current time
    start_time = int(time.time() * 1000)
    
    for i in range(num_snapshots):
        timestamp = start_time + (i * interval_ms)
        snapshot = generator.generate_snapshot(timestamp)
        snapshots.append(snapshot)
        
        # Progress indicator
        if i % 1000 == 0:
            progress = (i / num_snapshots) * 100
            print(f"   Progress: {progress:.1f}% ({i:,}/{num_snapshots:,})")
    
    print("✅ Data generation complete!")
    return snapshots

def save_test_data(snapshots: List[Dict], filename: str = "logs/test_orderbook_data.json"):
    """Save generated data to JSON file"""
    print(f"💾 Saving data to {filename}...")
    
    # Create metadata
    metadata = {
        'generated_at': datetime.now().isoformat(),
        'duration_hours': len(snapshots) * 100 / (1000 * 60 * 60),  # Convert to hours
        'interval_ms': 100,
        'num_snapshots': len(snapshots),
        'timeframe_coverage': {
            '100ms': len(snapshots),
            '500ms': len(snapshots) // 5,
            '1sec': len(snapshots) // 10,
            '1min': len(snapshots) // 600,
            '5min': len(snapshots) // 3000,
            '15min': len(snapshots) // 9000,
            '60min': len(snapshots) // 36000
        }
    }
    
    # Save to file
    output_data = {
        'metadata': metadata,
        'snapshots': snapshots
    }
    
    with open(filename, 'w') as f:
        json.dump(output_data, f, indent=2)
    
    print(f"✅ Saved {len(snapshots):,} snapshots to {filename}")
    print(f"   File size: ~{len(json.dumps(output_data)) / 1024:.1f} KB")
    
    # Print timeframe coverage
    print("\n📊 Timeframe Coverage:")
    for tf, count in metadata['timeframe_coverage'].items():
        print(f"   {tf:>6}: {count:,} data points")

def main():
    """Main function"""
    print("🎯 Order Book Test Data Generator")
    print("=" * 50)
    
    # Generate 1 hour of data at 100ms intervals
    snapshots = generate_test_data(duration_hours=1, interval_ms=100)
    
    # Save to file
    save_test_data(snapshots)
    
    print("\n🚀 Ready for Fractal Zoom Testing!")
    print("   Use this data to test timeframe aggregation:")
    print("   - Load the JSON file in your C++ application")
    print("   - Feed it to the heatmap at 100ms intervals")
    print("   - Test zooming out to see aggregation in action")
    
    # Sample data preview
    if snapshots:
        print(f"\n📋 Sample Data Preview:")
        sample = snapshots[0]
        print(f"   Symbol: {sample['symbol']}")
        print(f"   Mid Price: ${sample['mid_price']:,.2f}")
        print(f"   Spread: ${sample['spread']:.2f}")
        print(f"   Bid Levels: {len(sample['bids'])}")
        print(f"   Ask Levels: {len(sample['asks'])}")

if __name__ == "__main__":
    main() 