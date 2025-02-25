#pragma once

#include <ydb/core/testlib/test_client.h>

#include <ydb/public/lib/experimental/ydb_experimental.h>
#include <ydb/public/lib/yson_value/ydb_yson_value.h>
#include <ydb/public/sdk/cpp/client/ydb_scheme/scheme.h>
#include <ydb/public/sdk/cpp/client/ydb_table/table.h>
#include <ydb/public/sdk/cpp/client/draft/ydb_scripting.h>

#include <library/cpp/yson/node/node_io.h>

#include <ydb/library/yql/core/issue/yql_issue.h>

#include <library/cpp/json/json_reader.h>
#include <library/cpp/testing/unittest/tests_data.h>
#include <library/cpp/testing/unittest/registar.h>
#include <library/cpp/yson/writer.h>

#define Y_UNIT_TEST_TWIN(N, OPT)                                                                                   \
    template <bool OPT>                                                                                            \
    struct TTestCase##N : public TCurrentTestCase {                                                                \
        TTestCase##N() : TCurrentTestCase() {                                                                      \
            if constexpr (OPT) { Name_ = #N "+" #OPT; } else { Name_ = #N "-" #OPT; }                              \
        }                                                                                                          \
        static THolder<NUnitTest::TBaseTestCase> CreateOn()  { return ::MakeHolder<TTestCase##N<true>>();  }       \
        static THolder<NUnitTest::TBaseTestCase> CreateOff() { return ::MakeHolder<TTestCase##N<false>>(); }       \
        void Execute_(NUnitTest::TTestContext&) override;                                                          \
    };                                                                                                             \
    struct TTestRegistration##N {                                                                                  \
        TTestRegistration##N() {                                                                                   \
            TCurrentTest::AddTest(TTestCase##N<true>::CreateOn);                                                   \
            TCurrentTest::AddTest(TTestCase##N<false>::CreateOff);                                                 \
        }                                                                                                          \
    };                                                                                                             \
    static TTestRegistration##N testRegistration##N;                                                               \
    template <bool OPT>                                                                                            \
    void TTestCase##N<OPT>::Execute_(NUnitTest::TTestContext& ut_context Y_DECLARE_UNUSED)

#define Y_UNIT_TEST_NEW_ENGINE(N) Y_UNIT_TEST_TWIN(N, UseNewEngine)

#define Y_UNIT_TEST_QUAD(N, OPT1, OPT2)                                                                                              \
    template<bool OPT1, bool OPT2> void N(NUnitTest::TTestContext&);                                                                 \
    struct TTestRegistration##N {                                                                                                    \
        TTestRegistration##N() {                                                                                                     \
            TCurrentTest::AddTest(#N "-" #OPT1 "-" #OPT2, static_cast<void (*)(NUnitTest::TTestContext&)>(&N<false, false>), false); \
            TCurrentTest::AddTest(#N "+" #OPT1 "-" #OPT2, static_cast<void (*)(NUnitTest::TTestContext&)>(&N<true, false>), false);  \
            TCurrentTest::AddTest(#N "-" #OPT1 "+" #OPT2, static_cast<void (*)(NUnitTest::TTestContext&)>(&N<false, true>), false);  \
            TCurrentTest::AddTest(#N "+" #OPT1 "+" #OPT2, static_cast<void (*)(NUnitTest::TTestContext&)>(&N<true, true>), false);   \
        }                                                                                                                            \
    };                                                                                                                               \
    static TTestRegistration##N testRegistration##N;                                                                                 \
    template<bool OPT1, bool OPT2>                                                                                                   \
    void N(NUnitTest::TTestContext&)

template <bool UseNewEngine, bool ForceVersionV1 = false>
TString Query(const TString& tmpl) {
    return TStringBuilder()
        << (ForceVersionV1 ? "--!syntax_v1\n" : "")
        << "PRAGMA Kikimr.UseNewEngine = '" << (UseNewEngine ? "true" : "false") << "';" << Endl
        //<< (UseNewEngine ? "PRAGMA Kikimr.UseNewEngine = 'true';" : "")
        << tmpl;
}

#define Q_(expr) Query<UseNewEngine, false>(expr)
#define Q1_(expr) Query<UseNewEngine, true>(expr)

namespace NKikimr {
namespace NKqp {

const TString KikimrDefaultUtDomainRoot = "Root";

TVector<NKikimrKqp::TKqpSetting> SyntaxV1Settings();

struct TKikimrSettings: public TTestFeatureFlagsHolder<TKikimrSettings> {
    NKikimrConfig::TAppConfig AppConfig;
    NKikimrPQ::TPQConfig PQConfig;
    TVector<NKikimrKqp::TKqpSetting> KqpSettings;
    TString AuthToken;
    TString DomainRoot = KikimrDefaultUtDomainRoot;
    ui32 NodeCount = 1;
    bool WithSampleTables = true;
    TDuration KeepSnapshotTimeout = TDuration::Zero();
    IOutputStream* LogStream = nullptr;

    TKikimrSettings()
    {
        // default value for tests, can be overwritten by SetFeatureFlags()
        SetEnableKqpSessionActor(false);
        this->SetEnableKqpScanQueryStreamLookup(true);
    }

    TKikimrSettings& SetEnableKqpSessionActor(bool enable) {
        AppConfig.MutableTableServiceConfig()->SetEnableKqpSessionActor(enable);
        return *this;
    }

    TKikimrSettings& SetAppConfig(const NKikimrConfig::TAppConfig& value) { AppConfig = value; return *this; }
    TKikimrSettings& SetFeatureFlags(const NKikimrConfig::TFeatureFlags& value) { FeatureFlags = value; return *this; }
    TKikimrSettings& SetPQConfig(const NKikimrPQ::TPQConfig& value) { PQConfig = value; return *this; };
    TKikimrSettings& SetKqpSettings(const TVector<NKikimrKqp::TKqpSetting>& value) { KqpSettings = value; return *this; }
    TKikimrSettings& SetAuthToken(const TString& value) { AuthToken = value; return *this; }
    TKikimrSettings& SetDomainRoot(const TString& value) { DomainRoot = value; return *this; }
    TKikimrSettings& SetNodeCount(ui32 value) { NodeCount = value; return *this; }
    TKikimrSettings& SetWithSampleTables(bool value) { WithSampleTables = value; return *this; }
    TKikimrSettings& SetKeepSnapshotTimeout(TDuration value) { KeepSnapshotTimeout = value; return *this; }
    TKikimrSettings& SetLogStream(IOutputStream* follower) { LogStream = follower; return *this; };
};

class TKikimrRunner {
public:
    TKikimrRunner(const TKikimrSettings& settings);

    TKikimrRunner(const TVector<NKikimrKqp::TKqpSetting>& kqpSettings, const TString& authToken = "",
        const TString& domainRoot = KikimrDefaultUtDomainRoot, ui32 nodeCount = 1);

    TKikimrRunner(const NKikimrConfig::TAppConfig& appConfig, const TString& authToken = "",
        const TString& domainRoot = KikimrDefaultUtDomainRoot, ui32 nodeCount = 1);

    TKikimrRunner(const NKikimrConfig::TAppConfig& appConfig, const TVector<NKikimrKqp::TKqpSetting>& kqpSettings,
        const TString& authToken = "", const TString& domainRoot = KikimrDefaultUtDomainRoot, ui32 nodeCount = 1);

    TKikimrRunner(const NKikimrConfig::TFeatureFlags& featureFlags, const TString& authToken = "",
        const TString& domainRoot = KikimrDefaultUtDomainRoot, ui32 nodeCount = 1);

    TKikimrRunner(const TString& authToken = "", const TString& domainRoot = KikimrDefaultUtDomainRoot,
        ui32 nodeCount = 1);

    ~TKikimrRunner() {
        Driver->Stop(true);
        Server.Reset();
        Client.Reset();
    }

    const TString& GetEndpoint() const { return Endpoint; }
    const NYdb::TDriver& GetDriver() const { return *Driver; }
    NYdb::NScheme::TSchemeClient GetSchemeClient() const { return NYdb::NScheme::TSchemeClient(*Driver); }
    Tests::TClient& GetTestClient() const { return *Client; }
    Tests::TServer& GetTestServer() const { return *Server; }

    NYdb::TDriverConfig GetDriverConfig() const { return DriverConfig; }

    NYdb::NTable::TTableClient GetTableClient() const {
        return NYdb::NTable::TTableClient(*Driver, NYdb::NTable::TClientSettings()
            .UseQueryCache(false));
    }

    bool IsUsingSnapshotReads() const {
        return Server->GetRuntime()->GetAppData().FeatureFlags.GetEnableMvccSnapshotReads();
    }

private:
    void Initialize(const TKikimrSettings& settings);
    void WaitForKqpProxyInit();
    void CreateSampleTables();

private:
    THolder<Tests::TServerSettings> ServerSettings;
    THolder<Tests::TServer> Server;
    THolder<Tests::TClient> Client;
    TPortManager PortManager;
    TString Endpoint;
    NYdb::TDriverConfig DriverConfig;
    THolder<NYdb::TDriver> Driver;
};

inline TKikimrRunner KikimrRunnerEnableSessionActor(bool enable, TVector<NKikimrKqp::TKqpSetting> kqpSettings = {},
    const NKikimrConfig::TAppConfig& appConfig = {})
{
    auto settings = TKikimrSettings()
        .SetAppConfig(appConfig)
        .SetEnableKqpSessionActor(enable)
        .SetKqpSettings(kqpSettings);

    return TKikimrRunner{settings};
}

struct TCollectedStreamResult {
    TString ResultSetYson;
    TMaybe<TString> PlanJson;
    TMaybe<Ydb::TableStats::QueryStats> QueryStats;
    ui64 RowsCount = 0;
};

TCollectedStreamResult CollectStreamResult(NYdb::NExperimental::TStreamPartIterator& it);
TCollectedStreamResult CollectStreamResult(NYdb::NTable::TScanQueryPartIterator& it);

enum class EIndexTypeSql {
    Global,
    GlobalSync,
    GlobalAsync,
};

inline constexpr TStringBuf IndexTypeSqlString(EIndexTypeSql type) {
    switch (type) {
    case EIndexTypeSql::Global:
        return "GLOBAL";
    case EIndexTypeSql::GlobalSync:
        return "GLOBAL SYNC";
    case EIndexTypeSql::GlobalAsync:
        return "GLOBAL ASYNC";
    }
}

inline NYdb::NTable::EIndexType IndexTypeSqlToIndexType(EIndexTypeSql type) {
    switch (type) {
    case EIndexTypeSql::Global:
    case EIndexTypeSql::GlobalSync:
        return NYdb::NTable::EIndexType::GlobalSync;
    case EIndexTypeSql::GlobalAsync:
        return NYdb::NTable::EIndexType::GlobalAsync;
    }
}

TString ReformatYson(const TString& yson);
void CompareYson(const TString& expected, const TString& actual);
void CompareYson(const TString& expected, const NKikimrMiniKQL::TResult& actual);

bool HasIssue(const NYql::TIssues& issues, ui32 code,
    std::function<bool(const NYql::TIssue& issue)> predicate = {});

void PrintQueryStats(const NYdb::NTable::TDataQueryResult& result);

struct TExpectedTableStats {
    TMaybe<ui64> ExpectedReads;
    TMaybe<ui64> ExpectedUpdates;
    TMaybe<ui64> ExpectedDeletes;
};

void AssertTableStats(const NYdb::NTable::TDataQueryResult& result, TStringBuf table,
    const TExpectedTableStats& expectedStats);

inline void AssertTableReads(const NYdb::NTable::TDataQueryResult& result, TStringBuf table, ui64 expectedReads) {
    AssertTableStats(result, table, { .ExpectedReads = expectedReads });
}

NYdb::NTable::TDataQueryResult ExecQueryAndTestResult(NYdb::NTable::TSession& session, const TString& query,
    const NYdb::TParams& params, const TString& expectedYson);

inline NYdb::NTable::TDataQueryResult ExecQueryAndTestResult(NYdb::NTable::TSession& session, const TString& query,
    const TString& expectedYson)
{
    return ExecQueryAndTestResult(session, query, NYdb::TParamsBuilder().Build(), expectedYson);
}

TString StreamResultToYson(NYdb::NExperimental::TStreamPartIterator& it, TVector<TString>* profiles = nullptr);
TString StreamResultToYson(NYdb::NTable::TScanQueryPartIterator& it);
TString StreamResultToYson(NYdb::NScripting::TYqlResultPartIterator& it);
TString StreamResultToYson(NYdb::NTable::TTablePartIterator& it);

ui32 CountPlanNodesByKv(const NJson::TJsonValue& plan, const TString& key, const TString& value);
NJson::TJsonValue FindPlanNodeByKv(const NJson::TJsonValue& plan, const TString& key, const TString& value);
std::vector<NJson::TJsonValue> FindPlanNodes(const NJson::TJsonValue& plan, const TString& key);

TString ReadTableToYson(NYdb::NTable::TSession session, const TString& table);
TString ReadTablePartToYson(NYdb::NTable::TSession session, const TString& table);

inline void AssertSuccessResult(const NYdb::TStatus& result) {
    UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());
}

void CreateSampleTablesWithIndex(NYdb::NTable::TSession& session);

// KQP proxy needs to asynchronously receive tenants info before it is able to serve requests that have
// database name specified. Before that it returns errors.
// This method retries a simple query until it succeeds.
void WaitForKqpProxyInit(const NYdb::TDriver& driver);

void InitRoot(Tests::TServer::TPtr server, TActorId sender);

} // namespace NKqp
} // namespace NKikimr
