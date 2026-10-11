# CTest properties shared by test directories, including directories owned by
# other slices. Cross-directory test properties require CMake 3.28.
if(CMAKE_VERSION VERSION_LESS 3.28)
    message(WARNING "CTest resource and cost labels require CMake 3.28 or newer")
    return()
endif()

set(_sentinel_gpu_tests
    QsgComputeSpikeTests HeatmapGpuBinnerTests HeatmapTileNodeTests
    HeatmapLiveNodeTests LabItemTests ChartGoldenTests UgrGpuTests
    UgrGpuServiceTeardownGuardMalloc HeatmapGpuReadbackTeardownGuardMalloc
    HeatmapTileNodeTeardownGuardMalloc HeatmapSettingsUiTests ChartUiTests
    AgentApiInputTests MainWindowSymbolLifecycleTests ResearchUiTests)
set(_sentinel_server_tests
    CaptureApplicationTests ServerStartupTests LabOutputGuardTests
    RecordingServerStopTests HeatmapChunkWireTests ShadowRollerTests
    ServerFeedAdmissionTests BacktestCoreTests
    ResearchUiTests)
set(_sentinel_integration_tests
    ${_sentinel_server_tests} DeployRuntimeDryRun MarketDataEngineReconnectTests
    MarketDataEngineConnectTimeoutTests SentinelStreamClientWriteTests)
set(_sentinel_lint_tests
    AgentHostModeTests HeatmapPaletteTests HeatmapSettingsUiTests
    ServerFeedAdmissionTests DeployMarkerTests RollerCorruptBlockTests
    CaptureFanoutTests NoLocalStdTypeinfo)
set(_sentinel_processors_two
    RawCaptureTests CaptureVerifierScaleTests CaptureApplicationTests
    CaptureFanoutTests RollerTests ShadowRollerTests RollerCorruptBlockTests
    JournalAnchorTests)
set(_sentinel_fast_tests
    LogThrottleTests LogSinkTests MetricsRegistryTests
    LiveOrderBookTests MessageDispatcherTests SubscriptionManagerTests
    ProtocolValidationTests SentinelStreamClientParseHelperTests
    RecordingHistoryWireTests CoinbaseRestDeadlineTests
    CaptureRoutingTests TradingCoreTests AlgoTradingTests
    TimeframeAggregatorTests HmcolFormatTests SessionManagerTests
    RecordingCodecTests BookRecorderTests
    HeatmapTickPolicyTests LiveBookTickTests HeatmapTileTests
    RecordingPageTests PriceLadderTests
    ChunkFetcherTests HeatmapSpanPlannerTests CandleHistoryCacheTests
    FrameStatsWindowTests TimeAxisMappingTests ChartRasterTests
    ChartViewControllerTests ChartCameraTests
    GridViewStateTests CandleBackfillStateTests CandleDataSourceTests
    CandleSeriesBufferTests AxisLayoutTests PriceAxisModelTests
    TpoProfileModelTests HeatmapCellQueryTests HeatmapLabelLayoutTests
    AgentApiCodecTests SymbolSubscriptionManagerTests
    ExceptionTypeinfoTests)

foreach(_sentinel_area IN ITEMS core marketdata capture roller render agentapi
                                trading servermodel market_health fonts research_ui link)
    set(_sentinel_dir "${CMAKE_SOURCE_DIR}/tests/${_sentinel_area}")
    get_property(_sentinel_tests DIRECTORY "${_sentinel_dir}" PROPERTY TESTS)
    foreach(_sentinel_test IN LISTS _sentinel_tests)
        get_property(_sentinel_labels TEST "${_sentinel_test}" DIRECTORY "${_sentinel_dir}" PROPERTY LABELS)
        if(_sentinel_test IN_LIST _sentinel_fast_tests)
            list(APPEND _sentinel_labels fast)
        else()
            list(APPEND _sentinel_labels slow)
        endif()
        set(_sentinel_locks "")
        if(_sentinel_test IN_LIST _sentinel_gpu_tests)
            list(APPEND _sentinel_labels gpu)
            list(APPEND _sentinel_locks gpu)
        endif()
        if(_sentinel_test IN_LIST _sentinel_server_tests)
            list(APPEND _sentinel_locks server)
        endif()
        if(_sentinel_locks)
            set_tests_properties("${_sentinel_test}" DIRECTORY "${_sentinel_dir}"
                PROPERTIES RESOURCE_LOCK "${_sentinel_locks}")
        endif()
        if(_sentinel_test IN_LIST _sentinel_integration_tests)
            list(APPEND _sentinel_labels integration)
        endif()
        if(_sentinel_test IN_LIST _sentinel_lint_tests)
            list(APPEND _sentinel_labels lint)
        endif()
        if(_sentinel_test IN_LIST _sentinel_processors_two)
            set_tests_properties("${_sentinel_test}" DIRECTORY "${_sentinel_dir}"
                PROPERTIES PROCESSORS 2)
        endif()
        if(_sentinel_test STREQUAL PriceLadderTests)
            list(APPEND _sentinel_labels servermodel)
        endif()
        list(REMOVE_DUPLICATES _sentinel_labels)
        set_tests_properties("${_sentinel_test}" DIRECTORY "${_sentinel_dir}"
            PROPERTIES LABELS "${_sentinel_labels}")
    endforeach()
endforeach()
