#pragma once

// Feature flags for new rendering architecture
#ifndef SENTINEL_RENDER_V2
#define SENTINEL_RENDER_V2 1  // Default ON - enable new modular rendering
#endif

// Render strategy selection at compile time  
enum class RenderArchitecture {
    Legacy,      // Original monolithic UnifiedGridRenderer
    Modular      // New strategy-based architecture
};

constexpr RenderArchitecture CURRENT_RENDER_ARCH = 
    SENTINEL_RENDER_V2 ? RenderArchitecture::Modular : RenderArchitecture::Legacy;