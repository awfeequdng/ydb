#pragma once

#include <ydb/library/yql/providers/common/config/yql_dispatch.h>
#include <ydb/library/yql/providers/common/config/yql_setting.h>

#include <ydb/library/yql/core/yql_data_provider.h>

#include <library/cpp/string_utils/parse_size/parse_size.h>

#include <util/generic/size_literals.h>
#include <util/random/random.h>

namespace NYql {

struct TDqSettings {

    struct TDefault {
        static constexpr ui32 MaxTasksPerStage = 20U;
        static constexpr ui32 MaxTasksPerOperation = 70U;
        static constexpr bool EnablePorto = false;
        static constexpr ui64 DataSizePerJob = 128_MB;
        static constexpr ui64 MaxDataSizePerJob = 600_MB;
        static constexpr int MaxNetworkRetries = 5;
        static constexpr ui64 LiteralTimeout = 60000; // 1 minutes
        static constexpr ui64 TableTimeout = 600000; // 10 minutes
        static constexpr ui32 CloudFunctionConcurrency = 10;
        static constexpr ui64 ChannelBufferSize = 2000_MB;
        static constexpr ui64 OutputChunkMaxSize = 4_MB;
        static constexpr ui64 ChunkSizeLimit = 128_MB;
        static constexpr bool EnableDqReplicate = false;
    };

    using TPtr = std::shared_ptr<TDqSettings>;

    NCommon::TConfSetting<ui64, false> DataSizePerJob;
    NCommon::TConfSetting<ui64, false> MaxDataSizePerJob;
    NCommon::TConfSetting<ui32, false> MaxTasksPerStage;
    NCommon::TConfSetting<ui32, false> MaxTasksPerOperation;
    NCommon::TConfSetting<ui32, false> WorkersPerOperation;
    NCommon::TConfSetting<ui64, false> MaxDataSizePerQuery;
    NCommon::TConfSetting<bool, false> AnalyticsHopping;
    NCommon::TConfSetting<bool, false> AnalyzeQuery;
    NCommon::TConfSetting<int, false> _AnalyzeQueryPercentage;
    NCommon::TConfSetting<int, false> MaxRetries;
    NCommon::TConfSetting<int, false> MaxNetworkRetries;
    NCommon::TConfSetting<ui64, false> RetryBackoffMs;
    NCommon::TConfSetting<bool, false> CollectCoreDumps;
    NCommon::TConfSetting<TString, false> FallbackPolicy;
    NCommon::TConfSetting<ui64, false> PullRequestTimeoutMs;
    NCommon::TConfSetting<ui64, false> PingTimeoutMs;
    NCommon::TConfSetting<bool, false> UseSimpleYtReader;
    NCommon::TConfSetting<TString, false> OptLLVM;
    NCommon::TConfSetting<ui64, false> ChannelBufferSize;
    NCommon::TConfSetting<ui64, false> OutputChunkMaxSize;
    NCommon::TConfSetting<ui64, false> ChunkSizeLimit;
    NCommon::TConfSetting<NSize::TSize, false> MemoryLimit;
    NCommon::TConfSetting<ui64, false> _LiteralTimeout;
    NCommon::TConfSetting<ui64, false> _TableTimeout;
    NCommon::TConfSetting<ui64, false> _LongWorkersAllocationWarnTimeout;
    NCommon::TConfSetting<ui64, false> _LongWorkersAllocationFailTimeout;
    NCommon::TConfSetting<bool, false> EnableInsert;
    NCommon::TConfSetting<ui64, false> _AllResultsBytesLimit;
    NCommon::TConfSetting<ui64, false> _RowsLimitPerWrite;
    NCommon::TConfSetting<bool, false> EnableStrip;
    NCommon::TConfSetting<bool, false> EnableComputeActor;
    NCommon::TConfSetting<TString, false> ComputeActorType;
    NCommon::TConfSetting<bool, false> _EnablePorto;
    NCommon::TConfSetting<ui64, false> _PortoMemoryLimit;
    NCommon::TConfSetting<bool, false> EnableFullResultWrite;
    NCommon::TConfSetting<bool, false> _OneGraphPerQuery;
    NCommon::TConfSetting<TString, false> _FallbackOnRuntimeErrors;
    NCommon::TConfSetting<bool, false> _EnablePrecompute;
    NCommon::TConfSetting<bool, false> EnableDqReplicate;
    NCommon::TConfSetting<bool, false> _EnableGraceJoin;
    NCommon::TConfSetting<TString, false> WatermarksMode;
    NCommon::TConfSetting<ui64, false> WatermarksGranularityMs;

    NCommon::TConfSetting<TString, false> WorkerFilter;

    // This options will be passed to executor_actor and worker_actor
    template <typename TProtoConfig>
    void Save(TProtoConfig& config) {
#define SAVE_SETTING(name) \
        if (this->name.Get()) { \
            auto* s = config.AddSettings(); \
            s->SetName(#name); \
            s->SetValue(ToString(*this->name.Get())); \
        }

        SAVE_SETTING(AnalyticsHopping);
        SAVE_SETTING(MaxRetries);
        SAVE_SETTING(MaxNetworkRetries);
        SAVE_SETTING(WorkersPerOperation);
        SAVE_SETTING(RetryBackoffMs);
        SAVE_SETTING(FallbackPolicy);
        SAVE_SETTING(CollectCoreDumps);
        SAVE_SETTING(PullRequestTimeoutMs);
        SAVE_SETTING(PingTimeoutMs);
        SAVE_SETTING(OptLLVM);
        SAVE_SETTING(ChannelBufferSize);
        SAVE_SETTING(OutputChunkMaxSize);
        SAVE_SETTING(MemoryLimit);
        SAVE_SETTING(_LiteralTimeout);
        SAVE_SETTING(_TableTimeout);
        SAVE_SETTING(_LongWorkersAllocationWarnTimeout);
        SAVE_SETTING(_LongWorkersAllocationFailTimeout);
        SAVE_SETTING(_AllResultsBytesLimit);
        SAVE_SETTING(_RowsLimitPerWrite);
        SAVE_SETTING(EnableComputeActor);
        SAVE_SETTING(_EnablePorto);
        SAVE_SETTING(_PortoMemoryLimit);
        SAVE_SETTING(EnableFullResultWrite);
        SAVE_SETTING(_FallbackOnRuntimeErrors);
        SAVE_SETTING(WorkerFilter);
        SAVE_SETTING(ComputeActorType);
        SAVE_SETTING(WatermarksMode);
        SAVE_SETTING(WatermarksGranularityMs);

#undef SAVE_SETTING
    }

    TDqSettings::TPtr WithFillSettings(const IDataProvider::TFillSettings& fillSettings) const {
        auto copy = std::make_shared<TDqSettings>(*this);
        if (fillSettings.RowsLimitPerWrite) {
            copy->_RowsLimitPerWrite = *fillSettings.RowsLimitPerWrite;
        }
        if (fillSettings.AllResultsBytesLimit) {
            copy->_AllResultsBytesLimit = *fillSettings.AllResultsBytesLimit;
        }

        return copy;
    }
};

struct TDqConfiguration: public TDqSettings, public NCommon::TSettingDispatcher {
    using TPtr = TIntrusivePtr<TDqConfiguration>;

    TDqConfiguration();
    TDqConfiguration(const TDqConfiguration&) = delete;

    template <class TProtoConfig>
    void Init(const TProtoConfig& config, const TString& userName)
    {
        // Init settings from config
        this->Dispatch(config.GetDefaultSettings(), userName);

        // TODO: drop after releasing new gateways config
        if (this->AnalyzeQuery.Get().Empty()) {
            int percent = 0;

            if ((percent = this->_AnalyzeQueryPercentage.Get().GetOrElse(0)) &&
                RandomNumber<ui8>(100) < percent)
            {
                this->Dispatch(NCommon::ALL_CLUSTERS, "AnalyzeQuery", "true", TDqConfiguration::EStage::STATIC);
            }

            for (const auto& userFromConfig : config.GetDefaultAnalyzeQueryForUsers()) {
                if (userFromConfig == userName) {
                    this->Dispatch(NCommon::ALL_CLUSTERS, "AnalyzeQuery", "true", TDqConfiguration::EStage::STATIC);
                    break;
                }
            }
        }

        this->FreezeDefaults();
    }
};

} //namespace NYql
