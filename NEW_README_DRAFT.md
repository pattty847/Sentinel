# 🚀 Sentinel: Professional Trading Terminal

> **Open-source market analysis platform with GPU-accelerated visualization and AI insights**

<p align="center">
  <img src="https://img.shields.io/badge/Architecture-GPU_Accelerated-purple" alt="Architecture">
  <img src="https://img.shields.io/badge/Design-Lock_Free_Pipeline-green" alt="Design">
  <img src="https://img.shields.io/badge/License-AGPL--3.0-blue" alt="License">
  <img src="https://img.shields.io/badge/Status-Active_Development-orange" alt="Status">
</p>

<div align="center">

**Professional crypto market analysis with real-time order book heatmaps, multi-timeframe aggregation, and upcoming AI-powered insights through CopeNet.**

**[📚 Docs](#documentation) • [🚀 Quick Start](#quick-start) • [🗺️ Roadmap](#roadmap) • [💬 CopeNet AI](#copenet-ai)**

</div>

---

## 🔥 **What is Sentinel?**

Sentinel is an **open-source trading terminal** designed to eventually provide professional market analysis tools typically found in expensive commercial platforms. Built with modern C++17/Qt6 and GPU acceleration, it aims to deliver high-performance real-time market visualization.

### **📊 See it in Action**

<table>
<tr>
<td width="50%">

**🌡️ Liquidity Heatmap**
<img width="100%" alt="BookMap-style Heatmap" src="https://github.com/user-attachments/assets/fd3a14c2-80e1-47d7-93b4-99d3623ba819" />
<sub>Real-time order book visualization with anti-spoofing detection</sub>

</td>
<td width="50%">

**📈 Trade Flow Analysis** 
<img width="100%" alt="Trade Flow Visualization" src="https://github.com/user-attachments/assets/542d2497-d4e3-43d9-9663-9fbb135423e5" />
<sub>Professional market microstructure analysis</sub>

</td>
</tr>
</table>

---

## 🎯 **Why Sentinel?**

- **🆓 Completely Free & Open Source** - No subscriptions, no hidden costs
- **⚡ Professional Architecture** - GPU-accelerated rendering with efficient data structures
- **🧩 Modular Architecture** - Extensible design for custom strategies and indicators
- **🤖 AI Integration** (Coming Soon) - Natural language market analysis with CopeNet

### **🏗️ Built for Performance**

- **⚡ GPU-Accelerated Rendering** with OpenGL backend
- **🎮 Smooth 60+ FPS** target for real-time visualization  
- **🧠 Low-Latency Architecture** designed for responsiveness
- **💾 Memory-Efficient Design** with optimized data structures
- **🔥 Lock-Free Data Pipelines** for multi-threaded performance

---

## 🌟 **Key Features**

### **📊 Professional Market Analysis**
- 🌡️ **BookMap-style Heatmaps**: Dense order book visualization with time-based intensity
- 🕵️ **Anti-Spoofing Detection**: AI-powered fake liquidity filtering
- 📈 **Multi-Timeframe Analysis**: 100ms → 10s temporal aggregation
- 🎯 **Volume-at-Price**: Real-time depth analysis with precision targeting

### **🏗️ Modern Architecture**
- ⚡ **GPU Acceleration**: Direct-to-hardware rendering via Qt Scene Graph
- 🧩 **Modular Design**: Strategy-pattern rendering (Heatmap, Trade Flow, Candles)
- 🎮 **Real-time Processing**: Lock-free data pipelines with zero-malloc hot paths
- 🔄 **Cross-Platform**: Windows, macOS, Linux with modern build system

### **🤖 CopeNet AI (Coming Soon)**
- 💬 **Chat with Your Data**: "Is this whale trying to manipulate the price?"
- 🧠 **Market Insights**: AI explains what's happening in your viewport
- 📊 **Pattern Recognition**: Automated detection of market microstructure events
- 🚨 **Smart Alerts**: Context-aware notifications for trading opportunities

---

## 🚀 **Quick Start**

### **One-Line Install**
```bash
# Clone and build (works on Windows/macOS/Linux)
git clone https://github.com/your-repo/Sentinel.git && cd Sentinel
cmake --preset [windows-mingw|mac-clang|linux-gcc]
cmake --build --preset [windows-mingw|mac-clang|linux-gcc]
```

### **Requirements**
- **C++20** compiler (GCC 10+, Clang 12+, MSVC 19.29+)  
- **Qt 6.5+** for GUI framework
- **CMake 3.25+** for build system
- **OpenGL 3.3+** for GPU acceleration

*Full setup guide: [📖 Building Guide](docs/user/BUILDING.md)*

---

## 🗺️ **Roadmap**

### **✅ Phase 1-8: Foundation (COMPLETE)**
- ✅ High-performance streaming WebSocket client
- ✅ V2 modular rendering architecture with GPU acceleration
- ✅ Professional order book heatmap visualization  
- ✅ Cross-platform build system with CMake presets
- ✅ Anti-spoofing detection with persistence analysis

### **🔥 Phase 9: Advanced Visualization (IN PROGRESS)**
- 🚧 **Candlestick Charts** with volume integration
- 🚧 **Trade Bubble Visualization** showing size/aggression
- 🚧 **Volume Profile** horizontal distribution analysis
- 🚧 **Market Replay** historical data visualization

### **🤖 Phase 10: CopeNet AI Integration**
- 🔜 **Chat Interface** for real-time market analysis
- 🔜 **Pattern Recognition** ML models for market events
- 🔜 **Natural Language Queries** ("Show me all whale trades > $1M")
- 🔜 **Predictive Insights** based on order book dynamics

### **🌟 Phase 11: Professional Features**
- 🔜 **Multi-Symbol Support** (BTC, ETH, SOL, etc.)
- 🔜 **Custom Indicators** user-defined technical analysis
- 🔜 **Alert System** with webhooks and notifications
- 🔜 **Portfolio Integration** with trading APIs

*Full roadmap: [🗺️ ROADMAP.md](ROADMAP.md)*

---

## 🌟 **Key Advantages**

| Feature | Sentinel Benefits |
|---------|------------------|
| **Open Source** | Complete control over your trading environment |
| **Performance** | GPU-accelerated with 2.27M trades/sec capacity |
| **Modularity** | Extensible architecture for custom indicators |
| **AI Integration** | CopeNet for natural language market analysis |
| **No Vendor Lock-in** | Direct data feeds, no subscription dependencies |

**Professional market analysis platform designed for traders who value performance, transparency, and customization.**

---

## 🏗️ **Technical Architecture**

Sentinel is designed with performance in mind from the ground up:

- **🔄 Triple-Buffer Rendering**: Smooth frame delivery with zero tearing
- **📊 VBO-Based Graphics**: Direct GPU memory for efficient rendering  
- **🧵 Lock-Free Queues**: SPSC ring buffers for thread communication
- **⚡ Zero-Malloc Hot Paths**: Memory allocation avoided in critical loops
- **📈 Modular Rendering**: Strategy pattern for different visualization modes

*Technical details: [🏛️ System Architecture](docs/SYSTEM_ARCHITECTURE.md) • [⚙️ V2 Rendering Architecture](docs/V2_RENDERING_ARCHITECTURE.md)*

---

## 🤖 **CopeNet AI Preview**

Imagine talking to your charts like this:

```
You: "Is that big green candle just market buy orders or manipulation?"

CopeNet: "Analyzing the order book shows 15 large market buys totaling 
         $2.3M over 200ms, but I also detect 8 fake walls that 
         disappeared right before the pump. This looks like coordinated 
         manipulation with a 87% confidence score."

You: "Should I be worried about that 500 BTC bid wall?"

CopeNet: "That wall has only been there for 12 seconds and the address 
         has a history of spoofing. I'm seeing similar patterns from 
         3 other addresses. Persistence ratio is only 0.23 - likely fake."
```

🚧 **Status**: In development. Join our Discord for early access!

---

## 📚 **Documentation**

### **👤 For Users**
- [🚀 Quick Start Guide](docs/user/QUICKSTART.md)
- [📊 Feature Overview](docs/user/FEATURES.md) 
- [🏗️ Building & Installation](docs/user/BUILDING.md)
- [❓ FAQ & Troubleshooting](docs/user/FAQ.md)

### **🛠️ For Developers**
- [🏛️ System Architecture](docs/SYSTEM_ARCHITECTURE.md)
- [⚙️ V2 Rendering Architecture](docs/V2_RENDERING_ARCHITECTURE.md)
- [🤝 Contributing Guide](docs/developer/CONTRIBUTING.md)
- [🔧 API Documentation](docs/developer/API.md)
- [🧪 Testing Guide](docs/developer/TESTING.md)

---

## 🤝 **Contributing**

We welcome contributions! Sentinel is built by traders, for traders.

**Ways to help:**
- 🐛 **Bug Reports**: Found an issue? Open an issue!
- 🚀 **Feature Requests**: Need a specific indicator? Let us know!
- 💻 **Code Contributions**: Check our [🤝 Contributing Guide](docs/developer/CONTRIBUTING.md)
- 📖 **Documentation**: Help improve our docs
- 💰 **Sponsorship**: Support development costs

---

## 📜 **License**

Licensed under **GNU Affero General Public License v3.0 (AGPL-3.0)**.

**TL;DR**: Free for personal/educational use. If you build a commercial service with Sentinel, you must open source your modifications.

---

## 🌟 **Star History**

Show some love if you believe in open-source trading technology! ⭐

<div align="center">

[![Star History Chart](https://api.star-history.com/svg?repos=your-repo/Sentinel&type=Date)](https://star-history.com/#your-repo/Sentinel&Date)

**[⭐ Star this repo](https://github.com/your-repo/Sentinel) if you support professional open-source trading tools!**

</div>

---

<div align="center">

**Built with ⚡ by traders who believe in open-source professional tools**

[🐦 Twitter](https://twitter.com/your-handle) • [💬 Discord](https://discord.gg/your-invite) • [📧 Email](mailto:your-email)

</div>