#include <ydb/core/kqp/ut/common/kqp_ut_common.h>

#include <ydb/core/client/minikql_compile/mkql_compile_service.h>
#include <ydb/core/kqp/kqp_impl.h>
#include <ydb/core/kqp/kqp_metadata_loader.h>
#include <ydb/core/kqp/host/kqp_host.h>

#include <ydb/public/sdk/cpp/client/ydb_proto/accessor.h>
#include <ydb/public/sdk/cpp/client/ydb_table/table.h>

#include <ydb/library/yql/core/services/mounts/yql_mounts.h>
#include <ydb/library/yql/providers/common/provider/yql_provider.h>

#include <library/cpp/json/json_reader.h>

#include <util/string/printf.h>

namespace NKikimr {
namespace NKqp {

using namespace NYdb;
using namespace NYdb::NTable;
using namespace NYdb::NScripting;

using NYql::TExprContext;
using NYql::TExprNode;

namespace {

constexpr const char* TestCluster = "local_ut";

TIntrusivePtr<NKqp::IKqpGateway> GetIcGateway(Tests::TServer& server) {
    auto counters = MakeIntrusive<TKqpRequestCounters>();
    counters->Counters = new TKqpCounters(server.GetRuntime()->GetAppData(0).Counters);
    counters->TxProxyMon = new NTxProxy::TTxProxyMon(server.GetRuntime()->GetAppData(0).Counters);
    std::shared_ptr<NYql::IKikimrGateway::IKqpTableMetadataLoader> loader = std::make_shared<TKqpTableMetadataLoader>(server.GetRuntime()->GetAnyNodeActorSystem(), false);
    return NKqp::CreateKikimrIcGateway(TestCluster, "/Root", std::move(loader), server.GetRuntime()->GetAnyNodeActorSystem(),
        server.GetRuntime()->GetNodeId(0), counters, MakeMiniKQLCompileServiceID());
}

TIntrusivePtr<IKqpHost> CreateKikimrQueryProcessor(TIntrusivePtr<IKqpGateway> gateway,
    const TString& cluster, NYql::IModuleResolver::TPtr moduleResolver,
    const NKikimr::NMiniKQL::IFunctionRegistry* funcRegistry = nullptr,
    const TVector<NKikimrKqp::TKqpSetting>& settings = {}, bool keepConfigChanges = false)
{

    NYql::TKikimrConfiguration::TPtr kikimrConfig = MakeIntrusive<NYql::TKikimrConfiguration>();
    auto defaultSettingsData = NResource::Find("kqp_default_settings.txt");
    TStringInput defaultSettingsStream(defaultSettingsData);
    NKikimrKqp::TKqpDefaultSettings defaultSettings;
    UNIT_ASSERT(TryParseFromTextFormat(defaultSettingsStream, defaultSettings));
    kikimrConfig->Init(defaultSettings.GetDefaultSettings(), cluster, settings, true);

    return NKqp::CreateKqpHost(gateway, cluster, "/Root", kikimrConfig, moduleResolver, funcRegistry,
        keepConfigChanges);
}

NYql::NNodes::TExprBase GetExpr(const TString& ast, NYql::TExprContext& ctx, NYql::IModuleResolver* moduleResolver) {
    NYql::TAstParseResult astRes = NYql::ParseAst(ast);
    YQL_ENSURE(astRes.IsOk());
    NYql::TExprNode::TPtr result;
    YQL_ENSURE(CompileExpr(*astRes.Root, result, ctx, moduleResolver));
    return NYql::NNodes::TExprBase(result);
}

void CreateTableWithIndexWithState(
    Tests::TServer& server,
    const TString& indexName,
    TIntrusivePtr<NKqp::IKqpGateway> gateway,
    NYql::TIndexDescription::EIndexState state)
{
    using NYql::TKikimrTableMetadataPtr;
    using NYql::TKikimrTableMetadata;
    using NYql::TKikimrColumnMetadata;

    TKikimrTableMetadataPtr metadata = new TKikimrTableMetadata(TestCluster, server.GetSettings().DomainName + "/IndexedTableWithState");
    metadata->Columns["key"] = TKikimrColumnMetadata("key", 0, "Uint32", false);
    metadata->Columns["value"] = TKikimrColumnMetadata("value", 0, "String", false);
    metadata->Columns["value2"] = TKikimrColumnMetadata("value2", 0, "String", false);
    metadata->ColumnOrder = {"key", "value", "value2"};
    metadata->Indexes.push_back(
        NYql::TIndexDescription(
            indexName,
            {"value"},
            TVector<TString>(),
            NYql::TIndexDescription::EType::GlobalSync,
            state,
            0,
            0,
            0
        )
    );
    metadata->KeyColumnNames.push_back("key");

    {
        auto result = gateway->CreateTable(metadata, true).ExtractValueSync();
        UNIT_ASSERT(result.Success());
    }
}

} // namespace


Y_UNIT_TEST_SUITE(KqpIndexMetadata) {
    Y_UNIT_TEST_TWIN(HandleNotReadyIndex, UseNewEngine) {
        using namespace NYql;
        using namespace NYql::NNodes;

        auto setting = NKikimrKqp::TKqpSetting();

        auto serverSettings = TKikimrSettings()
            .SetEnableMvcc(true)
            .SetEnableMvccSnapshotReads(true)
            .SetKqpSettings({setting});
        TKikimrRunner kikimr(serverSettings);

        auto& server = kikimr.GetTestServer();
        auto gateway = GetIcGateway(server);
        const TString& indexName = "value_index";

        CreateTableWithIndexWithState(server, indexName, gateway, NYql::TIndexDescription::EIndexState::NotReady);

        TExprContext moduleCtx;
        NYql::IModuleResolver::TPtr moduleResolver;
        YQL_ENSURE(GetYqlDefaultModuleResolver(moduleCtx, moduleResolver));
        auto qp = CreateKikimrQueryProcessor(gateway, TestCluster, moduleResolver,
            server.GetFunctionRegistry());

        {
            const TString query = Q_(R"(
                UPSERT INTO `/Root/IndexedTableWithState` (key, value, value2)
                    VALUES (1, "qq", "ww")
            )");

            auto explainResult = qp->SyncExplainDataQuery(query, true);

            UNIT_ASSERT_C(explainResult.Success(), explainResult.Issues().ToString());

            TExprContext exprCtx;
            VisitExpr(GetExpr(explainResult.QueryAst, exprCtx, moduleResolver.get()).Ptr(),
                [&indexName](const TExprNode::TPtr& exprNode) {
                    if (UseNewEngine) {
                        if (TMaybeNode<TKqpUpsertRows>(exprNode)) {
                            UNIT_ASSERT(!TKqpUpsertRows(exprNode).Table().Path().Value().Contains(indexName));
                        }
                        if (TMaybeNode<TKqpDeleteRows>(exprNode)) {
                            UNIT_ASSERT(!TKqpDeleteRows(exprNode).Table().Path().Value().Contains(indexName));
                        }
                    } else {
                        if (auto maybeupdate = TMaybeNode<TKiUpdateRow>(exprNode)) {
                            auto update = maybeupdate.Cast();
                            TStringBuf toUpdate = update.Table().Path().Value();
                            UNIT_ASSERT(!toUpdate.Contains(indexName));
                        }
                        if (auto maybeerase = TMaybeNode<TKiEraseRow>(exprNode)) {
                            auto erase = maybeerase.Cast();
                            TStringBuf toErase = erase.Table().Path().Value();
                            UNIT_ASSERT(!toErase.Contains(indexName));
                        }
                    }
                    return true;
                });
        }

        {
            const TString query = Q_("SELECT * FROM `/Root/IndexedTableWithState` VIEW value_index WHERE value = \"q\";");
            auto result = qp->SyncExplainDataQuery(query, true);
            UNIT_ASSERT(!result.Success());
            UNIT_ASSERT_C(result.Issues().ToString().Contains("No global indexes for table"), result.Issues().ToString());
        }
    }

    Y_UNIT_TEST_TWIN(HandleWriteOnlyIndex, UseNewEngine) {
        using namespace NYql;
        using namespace NYql::NNodes;

        auto setting = NKikimrKqp::TKqpSetting();
        auto serverSettings = TKikimrSettings()
            .SetEnableMvcc(true)
            .SetEnableMvccSnapshotReads(true)
            .SetKqpSettings({setting});
        TKikimrRunner kikimr(serverSettings);

        auto& server = kikimr.GetTestServer();
        auto gateway = GetIcGateway(server);
        const TString& indexName = "value_index";

        CreateTableWithIndexWithState(server, indexName, gateway, NYql::TIndexDescription::EIndexState::WriteOnly);

        TExprContext moduleCtx;
        NYql::IModuleResolver::TPtr moduleResolver;
        YQL_ENSURE(GetYqlDefaultModuleResolver(moduleCtx, moduleResolver));
        auto qp = CreateKikimrQueryProcessor(gateway, TestCluster, moduleResolver,
            server.GetFunctionRegistry());

        {
            const TString query = Q_(R"(
                UPSERT INTO `/Root/IndexedTableWithState` (key, value, value2)
                    VALUES (1, "qq", "ww");
            )");
            auto explainResult = qp->SyncExplainDataQuery(query, true);
            UNIT_ASSERT_C(explainResult.Success(), explainResult.Issues().ToString());

            TExprContext exprCtx;
            bool indexUpdated = false;
            bool indexCleaned = false;
            VisitExpr(GetExpr(explainResult.QueryAst, exprCtx, moduleResolver.get()).Ptr(),
                [&indexName, &indexUpdated, &indexCleaned](const TExprNode::TPtr& exprNode) mutable {
                    if (UseNewEngine) {
                        if (TMaybeNode<TKqpUpsertRows>(exprNode)) {
                            if (TKqpUpsertRows(exprNode).Table().Path().Value().Contains(indexName)) {
                                indexUpdated = true;
                            }
                        }
                        if (TMaybeNode<TKqpDeleteRows>(exprNode)) {
                            if (TKqpDeleteRows(exprNode).Table().Path().Value().Contains(indexName)) {
                                indexCleaned = true;
                            }
                        }
                    } else {
                        if (auto maybeupdate = TMaybeNode<TKiUpdateRow>(exprNode)) {
                            auto update = maybeupdate.Cast();
                            TStringBuf toUpdate = update.Table().Path().Value();
                            if (toUpdate.Contains(indexName)) {
                                indexUpdated = true;
                            }
                        }
                        if (auto maybeerase = TMaybeNode<TKiEraseRow>(exprNode)) {
                            auto erase = maybeerase.Cast();
                            TStringBuf toErase = erase.Table().Path().Value();
                            if (toErase.Contains(indexName)) {
                                indexCleaned = true;
                            }
                        }
                    }
                    return true;
                });
            UNIT_ASSERT(indexUpdated);
            UNIT_ASSERT(indexCleaned);
        }

        {
            const TString query = Q_("SELECT * FROM `/Root/IndexedTableWithState` VIEW value_index WHERE value = \"q\";");
            auto result = qp->SyncExplainDataQuery(query, true);
            UNIT_ASSERT(!result.Success());
            UNIT_ASSERT(result.Issues().ToString().Contains("is not ready to use"));
        }
    }
}

Y_UNIT_TEST_SUITE(KqpIndexes) {
    Y_UNIT_TEST_NEW_ENGINE(NullInIndexTableNoDataRead) {
        auto setting = NKikimrKqp::TKqpSetting();
        TKikimrRunner kikimr({setting});
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();
        CreateSampleTablesWithIndex(session);
        {
            NYdb::NTable::TExecDataQuerySettings execSettings;
            execSettings.CollectQueryStats(ECollectQueryStatsMode::Basic);

            const TString query(Q1_(R"(
                SELECT Key FROM `/Root/SecondaryKeys` VIEW Index WHERE Fk IS NULL;
            )"));

            auto result = session.ExecuteDataQuery(
                                     query,
                                     TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx(),
                                     execSettings)
                              .ExtractValueSync();
            UNIT_ASSERT_C(result.GetIssues().Empty(), result.GetIssues().ToString());
            UNIT_ASSERT(result.IsSuccess());
            UNIT_ASSERT_VALUES_EQUAL(NYdb::FormatResultSetYson(result.GetResultSet(0)), "[[#];[[7]]]");

            auto& stats = NYdb::TProtoAccessor::GetProto(*result.GetStats());
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases().size(), UseNewEngine ? 2 : 1);

            int phase = UseNewEngine ? 1 : 0;
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(phase).table_access().size(), 1);
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(phase).table_access(0).name(), "/Root/SecondaryKeys/Index/indexImplTable");
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(phase).table_access(0).reads().rows(), 2);

        }
    }

    Y_UNIT_TEST_NEW_ENGINE(NullInIndexTable) {
        auto setting = NKikimrKqp::TKqpSetting();
        TKikimrRunner kikimr({setting});
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();
        CreateSampleTablesWithIndex(session);

        {
            const TString query(Q1_(R"(
                SELECT * FROM `/Root/SecondaryKeys` VIEW Index WHERE Fk IS NULL;
            )"));

            auto result = session.ExecuteDataQuery(
                                     query,
                                     TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                              .ExtractValueSync();
            UNIT_ASSERT_C(result.GetIssues().Empty(), result.GetIssues().ToString());
            UNIT_ASSERT(result.IsSuccess());
            UNIT_ASSERT_VALUES_EQUAL(NYdb::FormatResultSetYson(result.GetResultSet(0)), "[[#;#;[\"Payload8\"]];[#;[7];[\"Payload7\"]]]");
        }
// We can't validate it now. Try to improve it after migrate to MVCC
#if 0
        if (UseNewEngine) {
            NKikimrMiniKQL::TResult result;
            auto& oldClient = kikimr.GetTestClient();
            bool success = oldClient.FlatQuery(
                "("
                    "(let row '('('Key (Int32 '7))))"
                    "(let pgmReturn (AsList"
                    "    (EraseRow '/Root/SecondaryKeys row)"
                    "))"
                    "(return pgmReturn)"
                ")",
            result);

            UNIT_ASSERT(success);

            {
                const TString query(Q_(R"(
                    SELECT * FROM `/Root/SecondaryKeys` : Index WHERE Fk IS NULL;
                )"));

                auto result = session.ExecuteDataQuery(
                                     query,
                                     TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                              .ExtractValueSync();
                UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), NYdb::EStatus::GENERIC_ERROR);
            }
        }
#endif
    }

    Y_UNIT_TEST_NEW_ENGINE(WriteWithParamsFieldOrder) {
        auto setting = NKikimrKqp::TKqpSetting();
        auto serverSettings = TKikimrSettings()
            .SetKqpSettings({setting});
        TKikimrRunner kikimr(serverSettings);
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        {
            const TString keyColumnName = "key";
            auto builder = TTableBuilder()
               .AddNullableColumn(keyColumnName, EPrimitiveType::Uint64);

            builder.AddNullableColumn("index_0", EPrimitiveType::Utf8);
            builder.AddSecondaryIndex("index_0_name", TVector<TString>{"index_0", keyColumnName});
            builder.AddNullableColumn("value", EPrimitiveType::Uint32);
            builder.SetPrimaryKeyColumns({keyColumnName});

            auto result = session.CreateTable("/Root/Test1", builder.Build()).GetValueSync();
            UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());
        }

        {
            const TString query1(Q1_(R"(
                DECLARE $items AS List<Struct<'key':Uint64,'index_0':Utf8,'value':Uint32>>;
                UPSERT INTO `/Root/Test1`
                    SELECT * FROM AS_TABLE($items);
            )"));

            TParamsBuilder builder;
            builder.AddParam("$items").BeginList()
                .AddListItem()
                    .BeginStruct()
                        .AddMember("key").Uint64(646464646464)
                        .AddMember("index_0").Utf8("SomeUtf8Data")
                        .AddMember("value").Uint32(323232)
                    .EndStruct()
                .EndList()
            .Build();

            static const TTxControl txControl = TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx();
            auto result = session.ExecuteDataQuery(query1, txControl, builder.Build()).GetValueSync();
            UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());
        }

        {
            const auto& yson = ReadTablePartToYson(session, "/Root/Test1");
            const TString expected = R"([[[646464646464u];["SomeUtf8Data"];[323232u]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }
        {
            const auto& yson = ReadTablePartToYson(session, "/Root/Test1/index_0_name/indexImplTable");
            const TString expected = R"([[["SomeUtf8Data"];[646464646464u]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }
    }

    Y_UNIT_TEST_TWIN(SelectConcurentTX, UseNewEngine) {
        auto setting = NKikimrKqp::TKqpSetting();
        auto serverSettings = TKikimrSettings()
            .SetEnableMvcc(true)
            .SetEnableMvccSnapshotReads(true)
            .SetKqpSettings({setting});
        TKikimrRunner kikimr(serverSettings);
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        {
            auto tableBuilder = db.GetTableBuilder();
            tableBuilder
                .AddNullableColumn("Key", EPrimitiveType::String)
                .AddNullableColumn("Index2", EPrimitiveType::String)
                .AddNullableColumn("Value", EPrimitiveType::String);
            tableBuilder.SetPrimaryKeyColumns(TVector<TString>{"Key"});
            tableBuilder.AddSecondaryIndex("Index", TVector<TString>{"Index2"});
            auto result = session.CreateTable("/Root/TestTable", tableBuilder.Build()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL(result.IsTransportError(), false);
            UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), EStatus::SUCCESS);
        }

        {
            const TString query1(Q_(R"(
                UPSERT INTO `/Root/TestTable` (Key, Index2, Value) VALUES
                ("Primary1", "Secondary1", "Value1"),
                ("Primary2", "Secondary2", "Value2"),
                ("Primary3", "Secondary3", "Value3");
            )"));

            auto result = session.ExecuteDataQuery(
                                 query1,
                                 TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                          .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
        }

        const TString query1(Q_(R"(
            SELECT Index2 FROM `/Root/TestTable` WHERE Key = "Primary1";
        )"));

        // Start tx1, select from table by pk (without touching index table) and without commit
        auto result1 = session.ExecuteDataQuery(
                                 query1,
                                 TTxControl::BeginTx(TTxSettings::SerializableRW()))
                          .ExtractValueSync();
        UNIT_ASSERT_VALUES_EQUAL(result1.GetStatus(), NYdb::EStatus::SUCCESS);
        {
            auto yson = NYdb::FormatResultSetYson(result1.GetResultSet(0));
            UNIT_ASSERT_VALUES_EQUAL(yson, R"([[["Secondary1"]]])");
        }

        {
            // In other tx, update string
            const TString query1(Q_(R"(
                UPSERT INTO `/Root/TestTable` (Key, Index2, Value) VALUES
                ("Primary1", "Secondary1New", "Value1New")
            )"));

            auto result = session.ExecuteDataQuery(
                                 query1,
                                 TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                          .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
        }

        const TString query2(Q1_(R"(
            SELECT Index2 FROM `/Root/TestTable` VIEW Index WHERE Index2 = "Secondary1";
        )"));

        UNIT_ASSERT(result1.GetTransaction());
        // Continue tx1, select from index table only
        auto result2 = session.ExecuteDataQuery(
                                 query2,
                                 TTxControl::Tx(result1.GetTransaction().GetRef()).CommitTx())
                          .ExtractValueSync();
        // read only tx should succeed in MVCC case
        UNIT_ASSERT_VALUES_EQUAL_C(result2.GetStatus(), NYdb::EStatus::SUCCESS, result2.GetIssues().ToString());
    }

    Y_UNIT_TEST_TWIN(SelectConcurentTX2, UseNewEngine) {
        auto setting = NKikimrKqp::TKqpSetting();
        auto serverSettings = TKikimrSettings()
            .SetEnableMvcc(true)
            .SetEnableMvccSnapshotReads(true)
            .SetKqpSettings({setting});
        TKikimrRunner kikimr(serverSettings);
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        {
            auto tableBuilder = db.GetTableBuilder();
            tableBuilder
                .AddNullableColumn("Key", EPrimitiveType::String)
                .AddNullableColumn("Index2", EPrimitiveType::String)
                .AddNullableColumn("Value", EPrimitiveType::String);
            tableBuilder.SetPrimaryKeyColumns(TVector<TString>{"Key"});
            tableBuilder.AddSecondaryIndex("Index", TVector<TString>{"Index2"});
            auto result = session.CreateTable("/Root/TestTable", tableBuilder.Build()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL(result.IsTransportError(), false);
            UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), EStatus::SUCCESS);
        }

        {
            const TString query1(Q_(R"(
                UPSERT INTO `/Root/TestTable` (Key, Index2, Value) VALUES
                ("Primary1", "Secondary1", "Value1"),
                ("Primary2", "Secondary2", "Value2"),
                ("Primary3", "Secondary3", "Value3");
            )"));

            auto result = session.ExecuteDataQuery(
                                 query1,
                                 TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                          .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
        }

        const TString query1 = Q1_(R"(
            SELECT Index2 FROM `/Root/TestTable` VIEW Index WHERE Index2 = "Secondary1";
        )");

        // Start tx1, select from table by index
        auto result1 = session.ExecuteDataQuery(
                                 query1,
                                 TTxControl::BeginTx(TTxSettings::SerializableRW()))
                          .ExtractValueSync();
        UNIT_ASSERT_VALUES_EQUAL(result1.GetStatus(), NYdb::EStatus::SUCCESS);
        {
            auto yson = NYdb::FormatResultSetYson(result1.GetResultSet(0));
            UNIT_ASSERT_VALUES_EQUAL(yson, R"([[["Secondary1"]]])");
        }

        {
            // In other tx, update string
            const TString query(Q_(R"(
                UPSERT INTO `/Root/TestTable` (Key, Index2, Value) VALUES
                ("Primary1", "Secondary1New", "Value1New")
            )"));

            auto result = session.ExecuteDataQuery(
                                 query,
                                 TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                          .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
        }

        const TString query2(Q_(R"(
            UPSERT INTO `/Root/TestTable` (Key, Index2, Value) VALUES
            ("Primary4", "Secondary4", "Value4")
        )"));

        UNIT_ASSERT(result1.GetTransaction());
        // Continue tx1, write to table should fail in both MVCC and non-MVCC scenarios
        auto result2 = session.ExecuteDataQuery(
                                 query2,
                                 TTxControl::Tx(result1.GetTransaction().GetRef()).CommitTx())
                          .ExtractValueSync();
        UNIT_ASSERT_VALUES_EQUAL_C(result2.GetStatus(), NYdb::EStatus::ABORTED, result2.GetIssues().ToString().c_str());
    }

    Y_UNIT_TEST_TWIN(UpsertWithoutExtraNullDelete, UseNewEngine) {
        auto setting = NKikimrKqp::TKqpSetting();
        auto serverSettings = TKikimrSettings()
            .SetEnableMvcc(true)
            .SetEnableMvccSnapshotReads(true)
            .SetKqpSettings({setting});
        TKikimrRunner kikimr(serverSettings);
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        {
            auto tableBuilder = db.GetTableBuilder();
            tableBuilder
                .AddNullableColumn("Key", EPrimitiveType::String)
                .AddNullableColumn("Index2", EPrimitiveType::String)
                .AddNullableColumn("Value", EPrimitiveType::String);
            tableBuilder.SetPrimaryKeyColumns(TVector<TString>{"Key"});
            tableBuilder.AddSecondaryIndex("Index", TVector<TString>{"Index2"});
            auto result = session.CreateTable("/Root/TestTable", tableBuilder.Build()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL(result.IsTransportError(), false);
            UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), EStatus::SUCCESS);
        }

        {
            const TString query1(Q_(R"(
                UPSERT INTO `/Root/TestTable` (Key, Index2, Value) VALUES
                ("Primary1", "Secondary1", "Value1");
            )"));

            NYdb::NTable::TExecDataQuerySettings execSettings;
            execSettings.CollectQueryStats(ECollectQueryStatsMode::Basic);
            auto result = session.ExecuteDataQuery(
                                 query1,
                                 TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx(),
                                 execSettings)
                          .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());

            {
                const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index/indexImplTable");
                const TString expected = R"([[["Secondary1"];["Primary1"]]])";
                UNIT_ASSERT_VALUES_EQUAL(yson, expected);
            }

            auto& stats = NYdb::TProtoAccessor::GetProto(*result.GetStats());

            if (UseNewEngine) {
                UNIT_ASSERT_VALUES_EQUAL(stats.query_phases().size(), 4);

                UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(1).table_access().size(), 1);
                UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(1).table_access(0).name(), "/Root/TestTable");

                UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(3).table_access().size(), 2);
                UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(3).table_access(0).name(), "/Root/TestTable");
                UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(3).table_access(0).updates().rows(), 1);

                UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(3).table_access(1).name(), "/Root/TestTable/Index/indexImplTable");
                UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(3).table_access(1).updates().rows(), 1);
                UNIT_ASSERT(!stats.query_phases(3).table_access(0).has_deletes());
            } else {
                UNIT_ASSERT_VALUES_EQUAL(stats.query_phases().size(), 3);

                UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(1).table_access().size(), 1);
                UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(1).table_access(0).name(), "/Root/TestTable");
                UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(1).table_access(0).reads().rows(), 0);

                UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(2).table_access().size(), 2);
                UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(2).table_access(0).name(), "/Root/TestTable");
                UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(2).table_access(0).updates().rows(), 1);

                UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(2).table_access(1).name(), "/Root/TestTable/Index/indexImplTable");
                UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(2).table_access(1).updates().rows(), 1);
                UNIT_ASSERT(!stats.query_phases(2).table_access(0).has_deletes());
            }
        }

        {
            const TString query1 = Q1_(R"(
                UPSERT INTO `/Root/TestTable` (Key, Index2, Value) VALUES
                ("Primary1", "Secondary1_1", "Value1");
            )");

            NYdb::NTable::TExecDataQuerySettings execSettings;
            execSettings.CollectQueryStats(ECollectQueryStatsMode::Basic);
            auto result = session.ExecuteDataQuery(
                                 query1,
                                 TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx(),
                                 execSettings)
                          .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
            auto& stats = NYdb::TProtoAccessor::GetProto(*result.GetStats());
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases().size(), UseNewEngine ? 4 : 3);

            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(1).table_access().size(), 1);
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(1).table_access(0).name(), "/Root/TestTable");
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(1).table_access(0).reads().rows(), 1);

            int idx = UseNewEngine ? 3 : 2;

            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(idx).table_access().size(), 2);

            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(idx).table_access(0).name(), "/Root/TestTable");
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(idx).table_access(0).updates().rows(), 1);

            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(idx).table_access(1).name(), "/Root/TestTable/Index/indexImplTable");
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(idx).table_access(1).updates().rows(), 1);
            UNIT_ASSERT(stats.query_phases(idx).table_access(1).has_deletes());
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(idx).table_access(1).deletes().rows(), 1);
        }

        {
            // Upsert without touching index
            const TString query2 = Q1_(R"(
                UPSERT INTO `/Root/TestTable` (Key, Value) VALUES
                ("Primary1", "Value1_1");
            )");

            NYdb::NTable::TExecDataQuerySettings execSettings;
            execSettings.CollectQueryStats(ECollectQueryStatsMode::Basic);
            auto result = session.ExecuteDataQuery(
                                 query2,
                                 TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx(),
                                 execSettings)
                          .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
            auto& stats = NYdb::TProtoAccessor::GetProto(*result.GetStats());
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases().size(), UseNewEngine ? 4 : 3);

            // One read from main table
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(1).table_access().size(), 1);
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(1).table_access(0).name(), "/Root/TestTable");
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(1).table_access(0).reads().rows(), 1);

            int idx = UseNewEngine ? 3 : 2;
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(idx).table_access().size(), 2);

            // One update of main table
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(idx).table_access(0).name(), "/Root/TestTable");
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(idx).table_access(0).updates().rows(), 1);

            // No touching index
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(idx).table_access(1).name(), "/Root/TestTable/Index/indexImplTable");
            if (!UseNewEngine) { // BUG
                UNIT_ASSERT(!stats.query_phases(idx).table_access(1).has_updates());
                UNIT_ASSERT(!stats.query_phases(idx).table_access(1).has_deletes());
            }

            {
                const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index/indexImplTable");
                const TString expected = R"([[["Secondary1_1"];["Primary1"]]])";
                UNIT_ASSERT_VALUES_EQUAL(yson, expected);
            }
        }

        {
            // Update without touching index
            const TString query2 = Q1_(R"(
                UPDATE `/Root/TestTable` ON (Key, Value) VALUES
                ("Primary1", "Value1_2");
            )");

            NYdb::NTable::TExecDataQuerySettings execSettings;
            execSettings.CollectQueryStats(ECollectQueryStatsMode::Basic);
            auto result = session.ExecuteDataQuery(
                                 query2,
                                 TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx(),
                                 execSettings)
                          .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
            auto& stats = NYdb::TProtoAccessor::GetProto(*result.GetStats());
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases().size(), UseNewEngine ? 4 : 2); // BUG

            int idx = UseNewEngine ? 1 : 0;

            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(idx).table_access().size(), 1);
            // One read of main table
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(idx).table_access(0).name(), "/Root/TestTable");
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(idx).table_access(0).reads().rows(), 1);

            // One update of index table
            idx += (UseNewEngine ? 2 : 1);
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(idx).table_access().size(), 1);
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(idx).table_access(0).name(), "/Root/TestTable");
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(idx).table_access(0).updates().rows(), 1);

            // Thats it, no phase for index table - we remove it on compile time

            {
                const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index/indexImplTable");
                const TString expected = R"([[["Secondary1_1"];["Primary1"]]])";
                UNIT_ASSERT_VALUES_EQUAL(yson, expected);
            }
        }

        {
            // Update without touching index
            const TString query2 = Q1_(R"(
                UPDATE `/Root/TestTable` SET Value = "Value1_3"
                WHERE Key = "Primary1";
            )");

            NYdb::NTable::TExecDataQuerySettings execSettings;
            execSettings.CollectQueryStats(ECollectQueryStatsMode::Basic);
            auto result = session.ExecuteDataQuery(
                                 query2,
                                 TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx(),
                                 execSettings)
                          .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
            auto& stats = NYdb::TProtoAccessor::GetProto(*result.GetStats());
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases().size(), 2);

            // One read of main table
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(0).table_access().size(), 1);
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(0).table_access(0).name(), "/Root/TestTable");
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(0).table_access(0).reads().rows(), 1);

            // One update of main table
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(1).table_access().size(), 1);
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(1).table_access(0).name(), "/Root/TestTable");
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(1).table_access(0).updates().rows(), 1);

            {
                const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index/indexImplTable");
                const TString expected = R"([[["Secondary1_1"];["Primary1"]]])";
                UNIT_ASSERT_VALUES_EQUAL(yson, expected);
            }
        }

    }

    Y_UNIT_TEST_TWIN(UpsertWithNullKeysSimple, UseNewEngine) {
        auto setting = NKikimrKqp::TKqpSetting();
        auto serverSettings = TKikimrSettings()
            .SetEnableMvcc(true)
            .SetEnableMvccSnapshotReads(true)
            .SetKqpSettings({ setting });
        TKikimrRunner kikimr(serverSettings);
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        {
            auto tableBuilder = db.GetTableBuilder();
            tableBuilder
                .AddNullableColumn("Key", EPrimitiveType::String)
                .AddNullableColumn("IndexColumn", EPrimitiveType::String)
                .AddNullableColumn("Value", EPrimitiveType::String);
            tableBuilder.SetPrimaryKeyColumns(TVector<TString>{"Key"});
            auto result = session.CreateTable("/Root/TestTable", tableBuilder.Build()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL(result.IsTransportError(), false);
            UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), EStatus::SUCCESS);
        }
        {
            const TString query1(Q_(R"(
                UPSERT INTO `/Root/TestTable` (Key, IndexColumn, Value) VALUES
                ("Primary 1", "Secondary 1", "Value 1"),
                (Null,        "Secondary 2", "Value 2");
            )"));

            auto result = session.ExecuteDataQuery(
                query1,
                TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
        }
        {
            TAlterTableSettings alterSettings;
            alterSettings.AppendAddIndexes({ TIndexDescription("IndexName", {"IndexColumn"}) });
            auto result = session.AlterTable("/Root/TestTable", alterSettings).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL(result.IsTransportError(), false);
            UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), EStatus::SUCCESS);
        }
        {
            const TString query(Q1_(R"(
                SELECT Value FROM `/Root/TestTable` VIEW IndexName WHERE IndexColumn = 'Secondary 2';
            )"));

            auto result = session.ExecuteDataQuery(
                query,
                TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
            UNIT_ASSERT_VALUES_EQUAL(NYdb::FormatResultSetYson(result.GetResultSet(0)), "[[[\"Value 2\"]]]");
        }
        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/IndexName/indexImplTable");
            const TString expected =
                R"([[["Secondary 1"];["Primary 1"]];)"
                R"([["Secondary 2"];#]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }
        {
            const TString query1(Q_(R"(
                UPSERT INTO `/Root/TestTable` (Key, IndexColumn, Value) VALUES
                ("Primary 3", "Secondary 3", "Value 3"),
                (Null,        "Secondary 4", "Value 4");
            )"));

            auto result = session.ExecuteDataQuery(
                query1,
                TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
        }
        {
            const TString query(Q1_(R"(
                SELECT Value FROM `/Root/TestTable` VIEW IndexName WHERE IndexColumn = 'Secondary 4';
            )"));

            auto result = session.ExecuteDataQuery(
                query,
                TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
            UNIT_ASSERT_VALUES_EQUAL(NYdb::FormatResultSetYson(result.GetResultSet(0)), "[[[\"Value 4\"]]]");
        }
        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/IndexName/indexImplTable");
            const TString expected =
                R"([[["Secondary 1"];["Primary 1"]];)"
                R"([["Secondary 3"];["Primary 3"]];)"
                R"([["Secondary 4"];#]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }

    }

    Y_UNIT_TEST_TWIN(UpsertWithNullKeysComplex, UseNewEngine) {
        auto setting = NKikimrKqp::TKqpSetting();
        auto serverSettings = TKikimrSettings()
            .SetEnableMvcc(true)
            .SetEnableMvccSnapshotReads(true)
            .SetKqpSettings({setting});
        TKikimrRunner kikimr(serverSettings);
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        {
            // Create table with 1 index
            auto tableBuilder = db.GetTableBuilder();
            tableBuilder
                .AddNullableColumn("Key", EPrimitiveType::String)
                .AddNullableColumn("IndexColumn1", EPrimitiveType::String)
                .AddNullableColumn("IndexColumn2", EPrimitiveType::String)
                .AddNullableColumn("Value", EPrimitiveType::String);
            tableBuilder.SetPrimaryKeyColumns(TVector<TString>{"Key", "IndexColumn1", "IndexColumn2"});
            tableBuilder.AddSecondaryIndex("IndexName1", TVector<TString>{"IndexColumn1"});
            auto result = session.CreateTable("/Root/TestTable", tableBuilder.Build()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL(result.IsTransportError(), false);
            UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), EStatus::SUCCESS);
        }
        {
            // Upsert rows including one with PK starting with Null
            const TString query1(Q_(R"(
                UPSERT INTO `/Root/TestTable` (Key, IndexColumn1, IndexColumn2, Value) VALUES
                ("Primary 1", "Secondary1 1", "Secondary2 1", "Value 1"),
                (Null,       "Secondary1 2", "Secondary2 2", "Value 2");
            )"));

            auto result = session.ExecuteDataQuery(
                                 query1,
                                 TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                          .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
        }
        {
            // Read row with PK starting with Null by index1
            const TString query(Q1_(R"(
                SELECT Value FROM `/Root/TestTable` VIEW IndexName1 WHERE IndexColumn1 = 'Secondary1 2';
            )"));

            auto result = session.ExecuteDataQuery(
                query,
                TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
            UNIT_ASSERT_VALUES_EQUAL(NYdb::FormatResultSetYson(result.GetResultSet(0)), "[[[\"Value 2\"]]]");
        }
        {
            // Both rows should be in index1 table
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/IndexName1/indexImplTable");
            const TString expected =
                R"([[["Secondary1 1"];["Primary 1"];["Secondary2 1"]];)"
                R"([["Secondary1 2"];#;["Secondary2 2"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }
        {
            // Add second index with alter
            TAlterTableSettings alterSettings;
            alterSettings.AppendAddIndexes({ TIndexDescription("IndexName2", {"IndexColumn2"}) });
            auto result = session.AlterTable("/Root/TestTable", alterSettings).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL(result.IsTransportError(), false);
            UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), EStatus::SUCCESS);
        }
        {
            // Read row with PK starting with Null by index2
            const TString query(Q1_(R"(
                SELECT Value FROM `/Root/TestTable` VIEW IndexName2 WHERE IndexColumn2 = 'Secondary2 2';
            )"));

            auto result = session.ExecuteDataQuery(
                query,
                TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
            UNIT_ASSERT_VALUES_EQUAL(NYdb::FormatResultSetYson(result.GetResultSet(0)), "[[[\"Value 2\"]]]");
        }
        {
            // Both rows should also be in index2 table
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/IndexName2/indexImplTable");
            const TString expected =
                R"([[["Secondary2 1"];["Primary 1"];["Secondary1 1"]];)"
                R"([["Secondary2 2"];#;["Secondary1 2"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }
        {
            // Upsert more rows including one with PK starting with Null
            const TString query1(Q_(R"(
                UPSERT INTO `/Root/TestTable` (Key, IndexColumn1, IndexColumn2, Value) VALUES
                ("Primary 3", "Secondary1 3", "Secondary2 3", "Value 3"),
                (Null,       "Secondary1 4", "Secondary2 4", "Value 4");
            )"));

            auto result = session.ExecuteDataQuery(
                query1,
                TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
        }
        {
            // Read recently added row with PK starting with Null by index2
            const TString query(Q1_(R"(
                SELECT Value FROM `/Root/TestTable` VIEW IndexName2 WHERE IndexColumn2 = 'Secondary2 4';
            )"));

            auto result = session.ExecuteDataQuery(
                query,
                TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
            UNIT_ASSERT_VALUES_EQUAL(NYdb::FormatResultSetYson(result.GetResultSet(0)), "[[[\"Value 4\"]]]");
        }
        {
            // All 4 rows should be in index1 table
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/IndexName1/indexImplTable");
            const TString expected =
                R"([[["Secondary1 1"];["Primary 1"];["Secondary2 1"]];)"
                R"([["Secondary1 2"];#;["Secondary2 2"]];)"
                R"([["Secondary1 3"];["Primary 3"];["Secondary2 3"]];)"
                R"([["Secondary1 4"];#;["Secondary2 4"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }
        {
            // All 4 rows should also be in index2 table
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/IndexName2/indexImplTable");
            const TString expected =
                R"([[["Secondary2 1"];["Primary 1"];["Secondary1 1"]];)"
                R"([["Secondary2 2"];#;["Secondary1 2"]];)"
                R"([["Secondary2 3"];["Primary 3"];["Secondary1 3"]];)"
                R"([["Secondary2 4"];#;["Secondary1 4"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }

    }

    Y_UNIT_TEST(KeyIndex) {
        auto setting = NKikimrKqp::TKqpSetting();
        auto serverSettings = TKikimrSettings()
            .SetEnableMvcc(true)
            .SetEnableMvccSnapshotReads(true)
            .SetKqpSettings({setting});
        TKikimrRunner kikimr(serverSettings);
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        // Table with complex index
        {
            auto tableBuilder = db.GetTableBuilder();
            tableBuilder
                .AddNullableColumn("Key", EPrimitiveType::String)
                .AddNullableColumn("Index", EPrimitiveType::String);
            tableBuilder.SetPrimaryKeyColumns(TVector<TString>{"Key"});
            tableBuilder.AddSecondaryIndex("Index", TVector<TString>{"Index", "Key"});
            auto result = session.CreateTable("/Root/TestTable", tableBuilder.Build()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL(result.IsTransportError(), false);
            UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), EStatus::SUCCESS);
        }

        {
            const TString query1(R"(
                PRAGMA Kikimr.UseNewEngine = 'false';
                UPSERT INTO `/Root/TestTable` (Key, Index) VALUES
                ("Primary1", "Secondary1");
            )");

            NYdb::NTable::TExecDataQuerySettings execSettings;
            execSettings.CollectQueryStats(ECollectQueryStatsMode::Basic);
            auto result = session.ExecuteDataQuery(
                                 query1,
                                 TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx(),
                                 execSettings)
                          .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
            auto& stats = NYdb::TProtoAccessor::GetProto(*result.GetStats());
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases().size(), 3);

            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(1).table_access().size(), 1);
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(1).table_access(0).name(), "/Root/TestTable");
            // Empty table no read in stats
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(1).table_access(0).reads().rows(), 0);

            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(2).table_access().size(), 2);

            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(2).table_access(0).name(), "/Root/TestTable");
            // One update
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(2).table_access(0).updates().rows(), 1);

            // Index update without detele
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(2).table_access(1).name(), "/Root/TestTable/Index/indexImplTable");
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(2).table_access(1).updates().rows(), 1);
            UNIT_ASSERT(!stats.query_phases(2).table_access(1).has_deletes());
            {
                const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index/indexImplTable");
                const TString expected = R"([[["Secondary1"];["Primary1"]]])";
                UNIT_ASSERT_VALUES_EQUAL(yson, expected);
            }
        }

        {
            const TString query1(R"(
                PRAGMA Kikimr.UseNewEngine = 'false';
                UPSERT INTO `/Root/TestTable` (Key, Index) VALUES
                ("Primary1", "Secondary1_1");
            )");

            NYdb::NTable::TExecDataQuerySettings execSettings;
            execSettings.CollectQueryStats(ECollectQueryStatsMode::Basic);
            auto result = session.ExecuteDataQuery(
                                 query1,
                                 TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx(),
                                 execSettings)
                          .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
            auto& stats = NYdb::TProtoAccessor::GetProto(*result.GetStats());
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases().size(), 3);

            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(1).table_access().size(), 1);
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(1).table_access(0).name(), "/Root/TestTable");
            // One read to find previous index value
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(1).table_access(0).reads().rows(), 1);

            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(2).table_access().size(), 2);

            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(2).table_access(0).name(), "/Root/TestTable");
            // Update main table
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(2).table_access(0).updates().rows(), 1);

            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(2).table_access(1).name(), "/Root/TestTable/Index/indexImplTable");
            // Update index with deletion
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(2).table_access(1).updates().rows(), 1);
            UNIT_ASSERT(stats.query_phases(2).table_access(1).has_deletes());
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(2).table_access(1).deletes().rows(), 1);
            {
                const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index/indexImplTable");
                const TString expected = R"([[["Secondary1_1"];["Primary1"]]])";
                UNIT_ASSERT_VALUES_EQUAL(yson, expected);
            }
        }

        {
            const TString query1(R"(
                PRAGMA Kikimr.UseNewEngine = 'false';
                UPDATE `/Root/TestTable` ON (Key, Index) VALUES
                ("Primary1", "Secondary1_2");
            )");

            NYdb::NTable::TExecDataQuerySettings execSettings;
            execSettings.CollectQueryStats(ECollectQueryStatsMode::Basic);
            auto result = session.ExecuteDataQuery(
                                 query1,
                                 TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx(),
                                 execSettings)
                          .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
            auto& stats = NYdb::TProtoAccessor::GetProto(*result.GetStats());
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases().size(), 3);

            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(1).table_access().size(), 1);
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(1).table_access(0).name(), "/Root/TestTable");
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(1).table_access(0).reads().rows(), 1);

            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(2).table_access().size(), 2);

            // Update main table
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(2).table_access(0).name(), "/Root/TestTable");
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(2).table_access(0).updates().rows(), 1);

            // Update index
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(2).table_access(1).name(), "/Root/TestTable/Index/indexImplTable");
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(2).table_access(1).updates().rows(), 1);
            UNIT_ASSERT(stats.query_phases(2).table_access(1).has_deletes());
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(2).table_access(1).deletes().rows(), 1);
            {
                const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index/indexImplTable");
                const TString expected = R"([[["Secondary1_2"];["Primary1"]]])";
                UNIT_ASSERT_VALUES_EQUAL(yson, expected);
            }
        }

        {
            const TString query1(R"(
                PRAGMA Kikimr.UseNewEngine = 'false';
                UPDATE `/Root/TestTable` SET Index = "Secondary1_3"
                WHERE Key = "Primary1";
            )");

            NYdb::NTable::TExecDataQuerySettings execSettings;
            execSettings.CollectQueryStats(ECollectQueryStatsMode::Basic);
            auto result = session.ExecuteDataQuery(
                                 query1,
                                 TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx(),
                                 execSettings)
                          .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
            auto& stats = NYdb::TProtoAccessor::GetProto(*result.GetStats());
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases().size(), 2);

            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(0).table_access().size(), 1);
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(0).table_access(0).name(), "/Root/TestTable");
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(0).table_access(0).reads().rows(), 1);

            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(1).table_access().size(), 2);

            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(1).table_access(0).name(), "/Root/TestTable");
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(1).table_access(0).updates().rows(), 1);

            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(1).table_access(1).name(), "/Root/TestTable/Index/indexImplTable");
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(1).table_access(1).updates().rows(), 1);
            UNIT_ASSERT(stats.query_phases(1).table_access(1).has_deletes());
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(1).table_access(1).deletes().rows(), 1);
            {
                const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index/indexImplTable");
                const TString expected = R"([[["Secondary1_3"];["Primary1"]]])";
                UNIT_ASSERT_VALUES_EQUAL(yson, expected);
            }
        }
    }

    Y_UNIT_TEST(KeyIndex2) {
        auto setting = NKikimrKqp::TKqpSetting();
        auto serverSettings = TKikimrSettings()
            .SetEnableMvcc(true)
            .SetEnableMvccSnapshotReads(true)
            .SetKqpSettings({setting});
        TKikimrRunner kikimr(serverSettings);
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        // pk is <Key,Index>
        // index is <Index,Key>
        {
            auto tableBuilder = db.GetTableBuilder();
            tableBuilder
                .AddNullableColumn("Key", EPrimitiveType::String)
                .AddNullableColumn("Index", EPrimitiveType::String);
            tableBuilder.SetPrimaryKeyColumns(TVector<TString>{"Key", "Index"});
            tableBuilder.AddSecondaryIndex("Index", TVector<TString>{"Index", "Key"});
            auto result = session.CreateTable("/Root/TestTable", tableBuilder.Build()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL(result.IsTransportError(), false);
            UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), EStatus::SUCCESS);
        }

        {
            const TString query1(R"(
                PRAGMA Kikimr.UseNewEngine = 'false';
                UPSERT INTO `/Root/TestTable` (Key, Index) VALUES
                ("Primary1", "Secondary1");
            )");

            NYdb::NTable::TExecDataQuerySettings execSettings;
            execSettings.CollectQueryStats(ECollectQueryStatsMode::Basic);
            auto result = session.ExecuteDataQuery(
                                 query1,
                                 TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx(),
                                 execSettings)
                          .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
            auto& stats = NYdb::TProtoAccessor::GetProto(*result.GetStats());
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases().size(), 3);

            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(1).table_access().size(), 1);
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(1).table_access(0).name(), "/Root/TestTable");
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(1).table_access(0).reads().rows(), 0);

            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(2).table_access().size(), 2);

            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(2).table_access(0).name(), "/Root/TestTable");
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(2).table_access(0).updates().rows(), 1);

            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(2).table_access(1).name(), "/Root/TestTable/Index/indexImplTable");
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(2).table_access(1).updates().rows(), 1);
            // Empty table without delete
            UNIT_ASSERT(!stats.query_phases(2).table_access(1).has_deletes());
            {
                const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index/indexImplTable");
                const TString expected = R"([[["Secondary1"];["Primary1"]]])";
                UNIT_ASSERT_VALUES_EQUAL(yson, expected);
            }
        }

        {
            // Upsert on new pk
            const TString query1(R"(
                PRAGMA Kikimr.UseNewEngine = 'false';
                UPSERT INTO `/Root/TestTable` (Key, Index) VALUES
                ("Primary1", "Secondary1_1");
            )");

            NYdb::NTable::TExecDataQuerySettings execSettings;
            execSettings.CollectQueryStats(ECollectQueryStatsMode::Basic);
            auto result = session.ExecuteDataQuery(
                                 query1,
                                 TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx(),
                                 execSettings)
                          .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
            auto& stats = NYdb::TProtoAccessor::GetProto(*result.GetStats());
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases().size(), 3);

            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(1).table_access().size(), 1);
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(1).table_access(0).name(), "/Root/TestTable");
            // read nothing
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(1).table_access(0).reads().rows(), 0);

            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(2).table_access().size(), 2);

            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(2).table_access(0).name(), "/Root/TestTable");
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(2).table_access(0).updates().rows(), 1);

            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(2).table_access(1).name(), "/Root/TestTable/Index/indexImplTable");
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(2).table_access(1).updates().rows(), 1);
            // no deletion
            UNIT_ASSERT(!stats.query_phases(2).table_access(1).has_deletes());
            {
                const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index/indexImplTable");
                const TString expected = R"([[["Secondary1"];["Primary1"]];[["Secondary1_1"];["Primary1"]]])";
                UNIT_ASSERT_VALUES_EQUAL(yson, expected);
            }
        }

        {
            // Update on non existing key - do nothing
            const TString query1(R"(
                PRAGMA Kikimr.UseNewEngine = 'false';
                UPDATE `/Root/TestTable` ON (Key, Index) VALUES
                ("Primary1", "Secondary1_2");
            )");

            NYdb::NTable::TExecDataQuerySettings execSettings;
            execSettings.CollectQueryStats(ECollectQueryStatsMode::Basic);
            auto result = session.ExecuteDataQuery(
                                 query1,
                                 TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx(),
                                 execSettings)
                          .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
            auto& stats = NYdb::TProtoAccessor::GetProto(*result.GetStats());
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases().size(), 2);

            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(0).table_access().size(), 1);
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(0).table_access(0).name(), "/Root/TestTable");
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(0).table_access(0).reads().rows(), 0);

            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(1).table_access().size(), 1);

            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(1).table_access(0).name(), "/Root/TestTable");
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(1).table_access(0).updates().rows(), 0);
            {
                const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index/indexImplTable");
                const TString expected = R"([[["Secondary1"];["Primary1"]];[["Secondary1_1"];["Primary1"]]])";
                UNIT_ASSERT_VALUES_EQUAL(yson, expected);
            }
        }
    }

    Y_UNIT_TEST(ReplaceWithoutExtraNullDelete) {
        auto setting = NKikimrKqp::TKqpSetting();
        auto serverSettings = TKikimrSettings()
            .SetEnableMvcc(true)
            .SetEnableMvccSnapshotReads(true)
            .SetKqpSettings({setting});
        TKikimrRunner kikimr(serverSettings);
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        {
            auto tableBuilder = db.GetTableBuilder();
            tableBuilder
                .AddNullableColumn("Key", EPrimitiveType::String)
                .AddNullableColumn("Index2", EPrimitiveType::String)
                .AddNullableColumn("Value", EPrimitiveType::String);
            tableBuilder.SetPrimaryKeyColumns(TVector<TString>{"Key"});
            tableBuilder.AddSecondaryIndex("Index", TVector<TString>{"Index2"});
            auto result = session.CreateTable("/Root/TestTable", tableBuilder.Build()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL(result.IsTransportError(), false);
            UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), EStatus::SUCCESS);
        }

        {
            const TString query1(R"(
                PRAGMA Kikimr.UseNewEngine = 'false';
                REPLACE INTO `/Root/TestTable` (Key, Index2, Value) VALUES
                ("Primary1", "Secondary1", "Value1");
            )");

            NYdb::NTable::TExecDataQuerySettings execSettings;
            execSettings.CollectQueryStats(ECollectQueryStatsMode::Basic);
            auto result = session.ExecuteDataQuery(
                                 query1,
                                 TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx(),
                                 execSettings)
                          .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
            auto& stats = NYdb::TProtoAccessor::GetProto(*result.GetStats());
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases().size(), 3);

            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(1).table_access().size(), 1);
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(1).table_access(0).name(), "/Root/TestTable");
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(1).table_access(0).reads().rows(), 0);

            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(2).table_access().size(), 2);

            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(2).table_access(0).name(), "/Root/TestTable");
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(2).table_access(0).updates().rows(), 1);

            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(2).table_access(1).name(), "/Root/TestTable/Index/indexImplTable");
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(2).table_access(1).updates().rows(), 1);
            UNIT_ASSERT(!stats.query_phases(2).table_access(1).has_deletes());
        }

        {
            const TString query1(R"(
                PRAGMA Kikimr.UseNewEngine = 'false';
                REPLACE INTO `/Root/TestTable` (Key, Index2, Value) VALUES
                ("Primary1", "Secondary1_1", "Value1");
            )");

            NYdb::NTable::TExecDataQuerySettings execSettings;
            execSettings.CollectQueryStats(ECollectQueryStatsMode::Basic);
            auto result = session.ExecuteDataQuery(
                                 query1,
                                 TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx(),
                                 execSettings)
                          .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
            auto& stats = NYdb::TProtoAccessor::GetProto(*result.GetStats());

            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases().size(), 3);

            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(1).table_access().size(), 1);
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(1).table_access(0).name(), "/Root/TestTable");
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(1).table_access(0).reads().rows(), 1);

            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(2).table_access().size(), 2);

            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(2).table_access(0).name(), "/Root/TestTable");
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(2).table_access(0).updates().rows(), 1);

            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(2).table_access(1).name(), "/Root/TestTable/Index/indexImplTable");
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(2).table_access(1).updates().rows(), 1);
            UNIT_ASSERT(stats.query_phases(2).table_access(1).has_deletes());
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(2).table_access(1).deletes().rows(), 1);
        }
    }

    Y_UNIT_TEST_TWIN(SecondaryIndexUpsert1DeleteUpdate, UseNewEngine) {
        auto setting = NKikimrKqp::TKqpSetting();
        auto serverSettings = TKikimrSettings()
            .SetEnableMvcc(true)
            .SetEnableMvccSnapshotReads(true)
            .SetKqpSettings({setting});
        TKikimrRunner kikimr(serverSettings);
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        const auto& config = kikimr.GetTestServer().GetSettings().AppConfig;
        auto& tableSettings = config.GetTableServiceConfig();
        bool useSchemeCacheMeta = tableSettings.GetUseSchemeCacheMetadata();

        {
            auto tableBuilder = db.GetTableBuilder();
            tableBuilder
                .AddNullableColumn("Key", EPrimitiveType::String)
                .AddNullableColumn("Index2", EPrimitiveType::String)
                .AddNullableColumn("Value", EPrimitiveType::String);
            tableBuilder.SetPrimaryKeyColumns(TVector<TString>{"Key"});
            tableBuilder.AddSecondaryIndex("Index", TVector<TString>{"Index2"});
            auto result = session.CreateTable("/Root/TestTable", tableBuilder.Build()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL(result.IsTransportError(), false);
            UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), EStatus::SUCCESS);
        }

        {
            const TString query1(Q_(R"(
                UPSERT INTO `/Root/TestTable` (Key, Index2, Value) VALUES
                ("Primary1", "SomeOldIndex", "SomeOldValue");
            )"));

            auto result = session.ExecuteDataQuery(
                                     query1,
                                     TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                              .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
        }

        const TString query1(Q_(R"(
            UPSERT INTO `/Root/TestTable` (Key, Index2, Value) VALUES
            ("Primary1", "Secondary1", "Value1"),
            ("Primary2", "Secondary2", "Value2"),
            ("Primary3", "Secondary3", "Value3");
        )"));

        auto result = session.ExecuteDataQuery(
                                 query1,
                                 TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                          .ExtractValueSync();
        UNIT_ASSERT(result.IsSuccess());

        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index/indexImplTable");
            const TString expected = R"([[["Secondary1"];["Primary1"]];[["Secondary2"];["Primary2"]];[["Secondary3"];["Primary3"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }

        {
            auto result = session.DescribeTable("/Root/TestTable/Index/indexImplTable").ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), NYdb::EStatus::SCHEME_ERROR);
        }

        {
            const TString query(Q_(R"(
                SELECT * FROM `/Root/TestTable/Index/indexImplTable`;
            )"));

            auto result = session.ExecuteDataQuery(
                                     query,
                                     TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                              .ExtractValueSync();
            // KIKIMR-7997
            UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(),
                useSchemeCacheMeta ? NYdb::EStatus::SCHEME_ERROR : NYdb::EStatus::GENERIC_ERROR);
        }

        {
            const TString query(Q1_(R"(
                SELECT Value FROM `/Root/TestTable` VIEW WrongView WHERE Index2 = 'Secondary2';
            )"));

            auto result = session.ExecuteDataQuery(
                                     query,
                                     TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                              .ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), NYdb::EStatus::SCHEME_ERROR);
        }

        {
            const TString query(Q1_(R"(
                SELECT Value FROM `/Root/TestTable` VIEW Index WHERE Index2 = 'Secondary2';
            )"));

            auto result = session.ExecuteDataQuery(
                                     query,
                                     TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                              .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
            UNIT_ASSERT_VALUES_EQUAL(NYdb::FormatResultSetYson(result.GetResultSet(0)), "[[[\"Value2\"]]]");
        }

        {
            const TString query(Q_(R"(
                DELETE FROM `/Root/TestTable` ON (Key) VALUES ('Primary1');
            )"));

            auto result = session.ExecuteDataQuery(
                                     query,
                                     TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                              .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
        }

        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index/indexImplTable");
            const TString expected = R"([[["Secondary2"];["Primary2"]];[["Secondary3"];["Primary3"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }

        {
            const TString query(Q_(R"(
                DELETE FROM `/Root/TestTable` WHERE Key = 'Primary2';
            )"));

            auto result = session.ExecuteDataQuery(
                                     query,
                                     TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                              .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
        }

        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index/indexImplTable");
            const TString expected = R"([[["Secondary3"];["Primary3"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }

        {
            const TString query(Q_(R"(
                UPDATE `/Root/TestTable` ON (Key, Index2) VALUES ('Primary3', 'Secondary3_1');
            )"));

            auto result = session.ExecuteDataQuery(
                                     query,
                                     TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                              .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
        }

        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable");
            const TString expected = R"([[["Primary3"];["Secondary3_1"];["Value3"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }

        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index/indexImplTable");
            const TString expected = R"([[["Secondary3_1"];["Primary3"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }

        {
            const TString query(Q_(R"(
                UPDATE `/Root/TestTable` SET Index2 = 'Secondary3_2' WHERE Key = 'Primary3';
            )"));

            auto result = session.ExecuteDataQuery(
                                     query,
                                     TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                              .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
        }

        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable");
            const TString expected = R"([[["Primary3"];["Secondary3_2"];["Value3"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }

        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index/indexImplTable");
            const TString expected = R"([[["Secondary3_2"];["Primary3"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }
    }

    Y_UNIT_TEST_TWIN(SecondaryIndexUpsert2Update, UseNewEngine) {
        auto setting = NKikimrKqp::TKqpSetting();
        auto serverSettings = TKikimrSettings()
            .SetEnableMvcc(true)
            .SetEnableMvccSnapshotReads(true)
            .SetKqpSettings({setting});
        TKikimrRunner kikimr(serverSettings);
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        {
            auto tableBuilder = db.GetTableBuilder();
            tableBuilder
                .AddNullableColumn("Key", EPrimitiveType::String)
                .AddNullableColumn("Index2", EPrimitiveType::String)
                .AddNullableColumn("Index2A", EPrimitiveType::String);
            tableBuilder.SetPrimaryKeyColumns(TVector<TString>{"Key"});
            tableBuilder.AddSecondaryIndex("Index", TVector<TString>{"Index2", "Index2A"});
            auto result = session.CreateTable("/Root/TestTable", tableBuilder.Build()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL(result.IsTransportError(), false);
            UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), EStatus::SUCCESS);
        }

        const TString query1(Q_(R"(
            UPSERT INTO `/Root/TestTable` (Key, Index2, Index2A) VALUES
            ("Primary1", "Secondary1", "Secondary1A"),
            ("Primary2", "Secondary2", "Secondary2A"),
            ("Primary3", "Secondary3", "Secondary3A");
        )"));

        auto result = session.ExecuteDataQuery(
                query1,
                TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
            .ExtractValueSync();
        UNIT_ASSERT(result.IsSuccess());

        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index/indexImplTable");
            const TString expected = R"([[["Secondary1"];["Secondary1A"];["Primary1"]];[["Secondary2"];["Secondary2A"];["Primary2"]];[["Secondary3"];["Secondary3A"];["Primary3"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }

        {
            const TString query(Q_(R"(
                UPDATE `/Root/TestTable` ON (Key, Index2) VALUES ('Primary1', 'Secondary1_1');
            )"));

            auto result = session.ExecuteDataQuery(
                                     query,
                                     TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                              .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
        }

        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable");
            const TString expected = R"([[["Primary1"];["Secondary1_1"];["Secondary1A"]];[["Primary2"];["Secondary2"];["Secondary2A"]];[["Primary3"];["Secondary3"];["Secondary3A"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }

        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index/indexImplTable");
            const TString expected = R"([[["Secondary1_1"];["Secondary1A"];["Primary1"]];[["Secondary2"];["Secondary2A"];["Primary2"]];[["Secondary3"];["Secondary3A"];["Primary3"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }

        {
            const TString query(Q_(R"(
                UPDATE `/Root/TestTable` SET Index2 = 'Secondary1_2' WHERE Key = 'Primary1';
            )"));

            auto result = session.ExecuteDataQuery(
                                     query,
                                     TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                              .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
        }

        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable");
            const TString expected = R"([[["Primary1"];["Secondary1_2"];["Secondary1A"]];[["Primary2"];["Secondary2"];["Secondary2A"]];[["Primary3"];["Secondary3"];["Secondary3A"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }

        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index/indexImplTable");
            const TString expected = R"([[["Secondary1_2"];["Secondary1A"];["Primary1"]];[["Secondary2"];["Secondary2A"];["Primary2"]];[["Secondary3"];["Secondary3A"];["Primary3"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }
    }

    Y_UNIT_TEST_TWIN(SecondaryIndexUpdateOnUsingIndex, UseNewEngine) {
        auto setting = NKikimrKqp::TKqpSetting();
        auto serverSettings = TKikimrSettings()
            .SetEnableMvcc(true)
            .SetEnableMvccSnapshotReads(true)
            .SetKqpSettings({setting});
        TKikimrRunner kikimr(serverSettings);
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        {
            auto tableBuilder = db.GetTableBuilder();
            tableBuilder
                .AddNullableColumn("Key", EPrimitiveType::String)
                .AddNullableColumn("Index2", EPrimitiveType::String)
                .AddNullableColumn("Value", EPrimitiveType::String);
            tableBuilder.SetPrimaryKeyColumns(TVector<TString>{"Key"});
            tableBuilder.AddSecondaryIndex("Index", "Index2");
            auto result = session.CreateTable("/Root/TestTable", tableBuilder.Build()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL(result.IsTransportError(), false);
            UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), EStatus::SUCCESS);
        }

        const TString query1(Q_(R"(
            UPSERT INTO `/Root/TestTable` (Key, Index2, Value) VALUES
            ("Primary1", "Secondary1", "Val1");
        )"));

        auto result = session.ExecuteDataQuery(
                query1,
                TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
            .ExtractValueSync();
        UNIT_ASSERT(result.IsSuccess());

        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index/indexImplTable");
            const TString expected = R"([[["Secondary1"];["Primary1"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }

        {

            const TString query(Q1_(R"(
                UPDATE `/Root/TestTable` ON (Key, Index2, Value)
                    (SELECT Key, Index2, 'Val1_1' as Value FROM `/Root/TestTable` VIEW Index WHERE Index2 = 'Secondary1');
            )"));

            auto result = session.ExecuteDataQuery(
                    query,
                    TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
        }

        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable");
            const TString expected = R"([[["Primary1"];["Secondary1"];["Val1_1"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }

        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index/indexImplTable");
            const TString expected = R"([[["Secondary1"];["Primary1"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }
    }

    Y_UNIT_TEST_TWIN(SecondaryIndexSelectUsingScripting, UseNewEngine) {
        auto setting = NKikimrKqp::TKqpSetting();
        auto serverSettings = TKikimrSettings()
            .SetEnableMvcc(true)
            .SetEnableMvccSnapshotReads(true)
            .SetKqpSettings({setting});
        TKikimrRunner kikimr(serverSettings);
        auto db = kikimr.GetTableClient();
        TScriptingClient client(kikimr.GetDriver());
        {
            auto session = db.CreateSession().GetValueSync().GetSession();
            const TString createTableSql(R"(
                --!syntax_v1
                CREATE TABLE `/Root/SharedHouseholds` (
                    guest_huid Uint64, guest_id Uint64, owner_huid Uint64, owner_id Uint64, household_id String,
                    PRIMARY KEY (guest_huid, owner_huid, household_id),
                    INDEX shared_households_owner_huid GLOBAL SYNC ON (`owner_huid`)
                );)");
            auto result = session.ExecuteSchemeQuery(createTableSql).GetValueSync();
            UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());
        }

        {
            const TString query(Q1_(R"(
                SELECT
                    guest_id
                FROM
                    SharedHouseholds VIEW shared_households_owner_huid
                WHERE
                    owner_huid == 1 AND
                    household_id == "1";
            )"));

            auto result = client.ExecuteYqlScript(query).GetValueSync();

            UNIT_ASSERT_C(result.GetIssues().Empty(), result.GetIssues().ToString());
            UNIT_ASSERT(result.IsSuccess());
            UNIT_ASSERT_VALUES_EQUAL(NYdb::FormatResultSetYson(result.GetResultSet(0)), "[]");
        }
    }

    Y_UNIT_TEST_TWIN(SecondaryIndexOrderBy, UseNewEngine) {
        auto setting = NKikimrKqp::TKqpSetting();
        auto serverSettings = TKikimrSettings()
            .SetEnableMvcc(true)
            .SetEnableMvccSnapshotReads(true)
            .SetKqpSettings({setting});
        TKikimrRunner kikimr(serverSettings);
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        {
            auto tableBuilder = db.GetTableBuilder();
            tableBuilder
                .AddNullableColumn("Key", EPrimitiveType::Int64)
                .AddNullableColumn("Index2", EPrimitiveType::Int64)
                .AddNullableColumn("Value", EPrimitiveType::String);
            tableBuilder.SetPrimaryKeyColumns(TVector<TString>{"Key"});
            tableBuilder.AddSecondaryIndex("Index", TVector<TString>{"Index2"});
            auto result = session.CreateTable("/Root/TestTable", tableBuilder.Build()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL(result.IsTransportError(), false);
            UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), EStatus::SUCCESS);
        }

        {
            const TString query1(Q_(R"(
                UPSERT INTO `/Root/TestTable` (Key, Index2, Value) VALUES
                (1, 1001, "Value1"),
                (2, 1002, "Value2"),
                (3, 1003, "Value3");
            )"));

            auto result = session.ExecuteDataQuery(
                                 query1,
                                 TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                          .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
        }

        {
            const TString query(Q1_(R"(
                SELECT * FROM `/Root/TestTable` VIEW Index as t ORDER BY t.Index2 DESC;
            )"));

           {
                auto result = session.ExplainDataQuery(
                    query)
                    .ExtractValueSync();

                UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString().c_str());

                if (UseNewEngine) {
                    UNIT_ASSERT_C(result.GetAst().Contains("'('\"Reverse\")"), result.GetAst());
                } else {
                    UNIT_ASSERT_C(result.GetAst().Contains("'\"Reverse\" (Bool '\"true\")"), result.GetAst());
                    UNIT_ASSERT_C(result.GetAst().Contains("Sort"), result.GetAst());
                    UNIT_ASSERT_C(!result.GetAst().Contains("PartialSort"), result.GetAst());
                }
            }

            {
                auto result = session.ExecuteDataQuery(
                    query,
                    TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                    .ExtractValueSync();
                UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString().c_str());

                UNIT_ASSERT_VALUES_EQUAL(NYdb::FormatResultSetYson(result.GetResultSet(0)), "[[[1003];[3];[\"Value3\"]];[[1002];[2];[\"Value2\"]];[[1001];[1];[\"Value1\"]]]");
            }
        }

        {
            const TString query(Q1_(R"(
                SELECT * FROM `/Root/TestTable` VIEW Index as t ORDER BY t.Index2 DESC LIMIT 2;
            )"));

           {
                auto result = session.ExplainDataQuery(
                    query)
                    .ExtractValueSync();

                UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), EStatus::SUCCESS);

                UNIT_ASSERT_C(result.GetAst().Contains("('\"ItemsLimit\""), result.GetAst());

                if (UseNewEngine) {
                    UNIT_ASSERT_C(result.GetAst().Contains("'('\"Reverse\")"), result.GetAst());
                } else {
                    UNIT_ASSERT_C(result.GetAst().Contains("'\"Reverse\" (Bool '\"true\")"), result.GetAst());
                    UNIT_ASSERT_C(result.GetAst().Contains("Sort"), result.GetAst());
                    UNIT_ASSERT_C(!result.GetAst().Contains("PartialSort"), result.GetAst());
                }
            }

            {
                auto result = session.ExecuteDataQuery(
                    query,
                    TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                    .ExtractValueSync();
                UNIT_ASSERT(result.IsSuccess());
                UNIT_ASSERT_VALUES_EQUAL(NYdb::FormatResultSetYson(result.GetResultSet(0)), "[[[1003];[3];[\"Value3\"]];[[1002];[2];[\"Value2\"]]]");
            }
        }

        {
            const TString query(Q1_(R"(
                SELECT Index2, Key FROM `/Root/TestTable` VIEW Index as t ORDER BY t.Index2 DESC LIMIT 2;
            )"));

           {
                auto result = session.ExplainDataQuery(
                    query)
                    .ExtractValueSync();

                UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), EStatus::SUCCESS);

                UNIT_ASSERT_C(result.GetAst().Contains("('\"ItemsLimit\""), result.GetAst());
                if (UseNewEngine) {
                    UNIT_ASSERT_C(result.GetAst().Contains("'('\"Reverse\")"), result.GetAst());
                } else {
                    UNIT_ASSERT_C(result.GetAst().Contains("'\"Reverse\" (Bool '\"true\")"), result.GetAst());
                    UNIT_ASSERT_C(result.GetAst().Contains("Sort"), result.GetAst());
                    UNIT_ASSERT_C(!result.GetAst().Contains("PartialSort"), result.GetAst());
                }
            }

            {
                auto result = session.ExecuteDataQuery(
                    query,
                    TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                    .ExtractValueSync();
                UNIT_ASSERT(result.IsSuccess());
                UNIT_ASSERT_VALUES_EQUAL(NYdb::FormatResultSetYson(result.GetResultSet(0)), "[[[1003];[3]];[[1002];[2]]]");
            }
        }

        {
            const TString query(Q1_(R"(
                SELECT * FROM `/Root/TestTable` VIEW Index as t WHERE t.Index2 < 1003 ORDER BY t.Index2 DESC;
            )"));

            {
                auto result = session.ExplainDataQuery(
                    query)
                    .ExtractValueSync();

                UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), EStatus::SUCCESS);

                if (UseNewEngine) {
                    UNIT_ASSERT_C(result.GetAst().Contains("'('\"Reverse\")"), result.GetAst());
                } else {
                    UNIT_ASSERT_C(result.GetAst().Contains("'\"Reverse\" (Bool '\"true\")"), result.GetAst());
                    UNIT_ASSERT_C(result.GetAst().Contains("Sort"), result.GetAst());
                    UNIT_ASSERT_C(!result.GetAst().Contains("PartialSort"), result.GetAst());
                }
            }

            {
                auto result = session.ExecuteDataQuery(
                    query,
                    TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                    .ExtractValueSync();
                UNIT_ASSERT(result.IsSuccess());

                UNIT_ASSERT_VALUES_EQUAL(NYdb::FormatResultSetYson(result.GetResultSet(0)), "[[[1002];[2];[\"Value2\"]];[[1001];[1];[\"Value1\"]]]");
            }
        }

        {
            const TString query(Q1_(R"(
                SELECT * FROM `/Root/TestTable` VIEW Index as t ORDER BY t.Index2;
            )"));

            {
                auto result = session.ExplainDataQuery(
                    query)
                    .ExtractValueSync();

                UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), EStatus::SUCCESS);

                if (UseNewEngine) {
                    UNIT_ASSERT_C(!result.GetAst().Contains("'('\"Reverse\")"), result.GetAst());
                } else {
                    UNIT_ASSERT_C(!result.GetAst().Contains("'\"Reverse\" (Bool '\"true\")"), result.GetAst());
                    UNIT_ASSERT_C(result.GetAst().Contains("Sort"), result.GetAst());
                    UNIT_ASSERT_C(!result.GetAst().Contains("PartialSort"), result.GetAst());
                }
            }

            {
                auto result = session.ExecuteDataQuery(
                    query,
                    TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                    .ExtractValueSync();
                UNIT_ASSERT(result.IsSuccess());
                UNIT_ASSERT_VALUES_EQUAL(NYdb::FormatResultSetYson(result.GetResultSet(0)), "[[[1001];[1];[\"Value1\"]];[[1002];[2];[\"Value2\"]];[[1003];[3];[\"Value3\"]]]");
            }
        }

        {
            const TString query(Q1_(R"(
                SELECT * FROM `/Root/TestTable` VIEW Index as t ORDER BY t.Index2 LIMIT 2;
            )"));

            {
                auto result = session.ExplainDataQuery(
                    query)
                    .ExtractValueSync();

                UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), EStatus::SUCCESS);

                if (UseNewEngine) {
                    UNIT_ASSERT_C(!result.GetAst().Contains("'('\"Reverse\")"), result.GetAst());
                    UNIT_ASSERT_C(result.GetAst().Contains("('\"ItemsLimit\""), result.GetAst());
                } else {
                    UNIT_ASSERT_C(!result.GetAst().Contains("'\"Reverse\" (Bool '\"true\")"), result.GetAst());
                    UNIT_ASSERT_C(result.GetAst().Contains("('\"ItemsLimit\""), result.GetAst());
                    UNIT_ASSERT_C(result.GetAst().Contains("Sort"), result.GetAst());
                    UNIT_ASSERT_C(!result.GetAst().Contains("PartialSort"), result.GetAst());
                }
            }

            {
                auto result = session.ExecuteDataQuery(
                    query,
                    TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                    .ExtractValueSync();
                UNIT_ASSERT(result.IsSuccess());
                UNIT_ASSERT_VALUES_EQUAL(NYdb::FormatResultSetYson(result.GetResultSet(0)), "[[[1001];[1];[\"Value1\"]];[[1002];[2];[\"Value2\"]]]");
            }
        }

        {
            const TString query(Q1_(R"(
                SELECT * FROM `/Root/TestTable` VIEW Index as t
                WHERE t.Index2 > 1001
                ORDER BY t.Index2 LIMIT 2;
            )"));

            {
                auto result = session.ExplainDataQuery(
                    query)
                    .ExtractValueSync();
                UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), EStatus::SUCCESS);

                if (UseNewEngine) {
                    UNIT_ASSERT_C(result.GetAst().Contains("('\"ItemsLimit\""), result.GetAst());
                    UNIT_ASSERT_C(!result.GetAst().Contains("'('\"Reverse\")"), result.GetAst());
                } else {
                    UNIT_ASSERT_C(result.GetAst().Contains("('\"ItemsLimit\""), result.GetAst());
                    UNIT_ASSERT_C(!result.GetAst().Contains("'\"Reverse\" (Bool '\"true\")"), result.GetAst());
                    UNIT_ASSERT_C(result.GetAst().Contains("Sort"), result.GetAst());
                    UNIT_ASSERT_C(!result.GetAst().Contains("PartialSort"), result.GetAst());
                }
            }

            {
                auto result = session.ExecuteDataQuery(
                    query,
                    TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                    .ExtractValueSync();
                UNIT_ASSERT(result.IsSuccess());
                UNIT_ASSERT_VALUES_EQUAL(NYdb::FormatResultSetYson(result.GetResultSet(0)), "[[[1002];[2];[\"Value2\"]];[[1003];[3];[\"Value3\"]]]");
            }
        }

        {
            const TString query(Q1_(R"(
                SELECT * FROM `/Root/TestTable` VIEW Index as t
                WHERE t.Index2 = 1002
                ORDER BY t.Index2 LIMIT 2;
            )"));

            {
                auto result = session.ExplainDataQuery(
                    query)
                    .ExtractValueSync();
                UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), EStatus::SUCCESS);

                if (UseNewEngine) {
                    UNIT_ASSERT_C(result.GetAst().Contains("('\"ItemsLimit\""), result.GetAst());
                    UNIT_ASSERT_C(!result.GetAst().Contains("'('\"Reverse\")"), result.GetAst());
                } else {
                    UNIT_ASSERT_C(result.GetAst().Contains("('\"ItemsLimit\""), result.GetAst());
                    UNIT_ASSERT_C(!result.GetAst().Contains("'\"Reverse\" (Bool '\"true\")"), result.GetAst());
                    UNIT_ASSERT_C(result.GetAst().Contains("Sort"), result.GetAst());
                    UNIT_ASSERT_C(!result.GetAst().Contains("PartialSort"), result.GetAst());
                }
            }

            {
                auto result = session.ExecuteDataQuery(
                    query,
                    TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                    .ExtractValueSync();
                UNIT_ASSERT(result.IsSuccess());
                UNIT_ASSERT_VALUES_EQUAL(NYdb::FormatResultSetYson(result.GetResultSet(0)), "[[[1002];[2];[\"Value2\"]]]");
            }
        }


        {
            const TString query(Q1_(R"(
                SELECT Index2, Key FROM `/Root/TestTable` VIEW Index as t
                WHERE t.Index2 > 1001
                ORDER BY t.Index2 LIMIT 2;
            )"));

            {
                auto result = session.ExplainDataQuery(
                    query)
                    .ExtractValueSync();
                UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), EStatus::SUCCESS);

                if (UseNewEngine) {
                    UNIT_ASSERT_C(result.GetAst().Contains("('\"ItemsLimit\""), result.GetAst());
                    UNIT_ASSERT_C(!result.GetAst().Contains("'('\"Reverse\")"), result.GetAst());
                } else {
                    UNIT_ASSERT_C(result.GetAst().Contains("('\"ItemsLimit\""), result.GetAst());
                    UNIT_ASSERT_C(!result.GetAst().Contains("'\"Reverse\" (Bool '\"true\")"), result.GetAst());
                    UNIT_ASSERT_C(result.GetAst().Contains("Sort"), result.GetAst());
                    UNIT_ASSERT_C(!result.GetAst().Contains("PartialSort"), result.GetAst());
                }
            }

            {
                auto result = session.ExecuteDataQuery(
                    query,
                    TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                    .ExtractValueSync();
                UNIT_ASSERT(result.IsSuccess());
                UNIT_ASSERT_VALUES_EQUAL(NYdb::FormatResultSetYson(result.GetResultSet(0)), "[[[1002];[2]];[[1003];[3]]]");
            }
        }

        {
            // Request by Key but using Index table, expect correct result
            const TString query(Q1_(R"(
                SELECT Index2, Key FROM `/Root/TestTable` VIEW Index as t
                WHERE t.Key > 1
                ORDER BY t.Index2 LIMIT 2;
            )"));

            {
                auto result = session.ExplainDataQuery(
                    query)
                    .ExtractValueSync();
                UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), EStatus::SUCCESS);

                if (UseNewEngine) {
                    UNIT_ASSERT_C(!result.GetAst().Contains("('\"ItemsLimit\""), result.GetAst());
                    UNIT_ASSERT_C(!result.GetAst().Contains("'('\"Reverse\")"), result.GetAst());
                } else {
                    UNIT_ASSERT_C(!result.GetAst().Contains("('\"ItemsLimit\""), result.GetAst());
                    UNIT_ASSERT_C(!result.GetAst().Contains("'\"Reverse\" (Bool '\"true\")"), result.GetAst());
                    UNIT_ASSERT_C(result.GetAst().Contains("Sort"), result.GetAst());
                    UNIT_ASSERT_C(!result.GetAst().Contains("PartialSort"), result.GetAst());
                }
            }

            {
                auto result = session.ExecuteDataQuery(
                    query,
                    TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                    .ExtractValueSync();
                UNIT_ASSERT(result.IsSuccess());
                UNIT_ASSERT_VALUES_EQUAL(NYdb::FormatResultSetYson(result.GetResultSet(0)), "[[[1002];[2]];[[1003];[3]]]");
            }
        }
    }

    Y_UNIT_TEST_TWIN(SecondaryIndexOrderBy2, UseNewEngine) {
        auto setting = NKikimrKqp::TKqpSetting();
        auto serverSettings = TKikimrSettings()
            .SetEnableMvcc(true)
            .SetEnableMvccSnapshotReads(true)
            .SetKqpSettings({setting});
        TKikimrRunner kikimr(serverSettings);
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        {
            auto tableBuilder = db.GetTableBuilder();
            tableBuilder
                .AddNullableColumn("id", EPrimitiveType::Uint64)
                .AddNullableColumn("customer", EPrimitiveType::Utf8)
                .AddNullableColumn("created", EPrimitiveType::Datetime)
                .AddNullableColumn("processed", EPrimitiveType::String);
            tableBuilder.SetPrimaryKeyColumns(TVector<TString>{"id"});
            tableBuilder.AddSecondaryIndex("ix_cust", TVector<TString>{"customer"});
            tableBuilder.AddSecondaryIndex("ix_cust2", TVector<TString>{"customer", "created"});
            tableBuilder.AddSecondaryIndex("ix_cust3", TVector<TString>{"customer", "created"}, TVector<TString>{"processed"});
            auto result = session.CreateTable("/Root/TestTable", tableBuilder.Build()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL(result.IsTransportError(), false);
            UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), EStatus::SUCCESS);
        }

        {
            const TString query1(Q_(R"(
                UPSERT INTO `/Root/TestTable` (id, customer, created, processed) VALUES
                (1, "Vasya", CAST('2020-01-01T00:00:01Z' as DATETIME), "Value1"),
                (2, "Vova",  CAST('2020-01-01T00:00:02Z' as DATETIME), "Value2"),
                (3, "Petya", CAST('2020-01-01T00:00:03Z' as DATETIME), "Value3"),
                (4, "Vasya", CAST('2020-01-01T00:00:04Z' as DATETIME), "Value4"),
                (5, "Vasya", CAST('2020-01-01T00:00:05Z' as DATETIME), "Value5");
            )"));

            auto result = session.ExecuteDataQuery(
                                 query1,
                                 TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                          .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
        }

        NYdb::NTable::TExecDataQuerySettings execSettings;
        execSettings.CollectQueryStats(ECollectQueryStatsMode::Basic);

        {
            const TString query(Q1_(R"(
                SELECT * FROM `/Root/TestTable` VIEW ix_cust as t WHERE t.customer = "Vasya"
                ORDER BY t.customer DESC, t.id DESC;
            )"));

           {
                auto result = session.ExplainDataQuery(
                    query)
                    .ExtractValueSync();

                UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString().c_str());

                if (UseNewEngine) {
                    UNIT_ASSERT_C(result.GetAst().Contains("'('\"Reverse\")"), result.GetAst());
                } else {
                    UNIT_ASSERT_C(result.GetAst().Contains("'\"Reverse\" (Bool '\"true\")"), result.GetAst());
                    UNIT_ASSERT_C(result.GetAst().Contains("Sort"), result.GetAst());
                    UNIT_ASSERT_C(!result.GetAst().Contains("PartialSort"), result.GetAst());
                }
            }

            {
                auto result = session.ExecuteDataQuery(
                    query,
                    TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx(), execSettings)
                    .ExtractValueSync();
                UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString().c_str());

                UNIT_ASSERT_VALUES_EQUAL(NYdb::FormatResultSetYson(result.GetResultSet(0)), "[[[1577836805u];[\"Vasya\"];[5u];[\"Value5\"]];[[1577836804u];[\"Vasya\"];[4u];[\"Value4\"]];[[1577836801u];[\"Vasya\"];[1u];[\"Value1\"]]]");

                auto& stats = NYdb::TProtoAccessor::GetProto(*result.GetStats());

                int indexPhaseId = 0;
                int tablePhaseId = 1;

                UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(tablePhaseId).table_access().size(), 1);
                UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(tablePhaseId).table_access(0).name(), "/Root/TestTable");
                UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(tablePhaseId).table_access(0).reads().rows(), 3);

                UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(indexPhaseId).table_access().size(), 1);
                UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(indexPhaseId).table_access(0).name(), "/Root/TestTable/ix_cust/indexImplTable");
                UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(indexPhaseId).table_access(0).reads().rows(), 3);
            }
        }

        {
            const TString query(Q1_(R"(
                SELECT * FROM `/Root/TestTable` VIEW ix_cust2 as t WHERE t.customer = "Vasya" ORDER BY t.customer DESC, t.created DESC LIMIT 2;
            )"));

           {
                auto result = session.ExplainDataQuery(
                    query)
                    .ExtractValueSync();

                UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString().c_str());

                if (UseNewEngine) {
                    UNIT_ASSERT_C(result.GetAst().Contains("'('\"ItemsLimit"), result.GetAst());
                    UNIT_ASSERT_C(result.GetAst().Contains("'('\"Reverse\")"), result.GetAst());
                } else {
                    UNIT_ASSERT_C(result.GetAst().Contains("'\"Reverse\" (Bool '\"true\")"), result.GetAst());
                    UNIT_ASSERT_C(result.GetAst().Contains("Sort"), result.GetAst());
                    UNIT_ASSERT_C(!result.GetAst().Contains("PartialSort"), result.GetAst());
                }
            }

            {
                auto result = session.ExecuteDataQuery(
                    query,
                    TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx(), execSettings)
                    .ExtractValueSync();
                UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString().c_str());

                UNIT_ASSERT_VALUES_EQUAL(NYdb::FormatResultSetYson(result.GetResultSet(0)), "[[[1577836805u];[\"Vasya\"];[5u];[\"Value5\"]];[[1577836804u];[\"Vasya\"];[4u];[\"Value4\"]]]");

                auto& stats = NYdb::TProtoAccessor::GetProto(*result.GetStats());

                int indexPhaseId = 0;
                int tablePhaseId = 1;

                UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(tablePhaseId).table_access().size(), 1);
                UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(tablePhaseId).table_access(0).name(), "/Root/TestTable");
                UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(tablePhaseId).table_access(0).reads().rows(), 2);

                UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(indexPhaseId).table_access().size(), 1);
                UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(indexPhaseId).table_access(0).name(), "/Root/TestTable/ix_cust2/indexImplTable");
                UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(indexPhaseId).table_access(0).reads().rows(), 2);
            }
        }

        {
            const TString query(Q1_(R"(
                SELECT * FROM `/Root/TestTable` VIEW ix_cust3 as t WHERE t.customer = "Vasya" ORDER BY t.customer DESC, t.created DESC LIMIT 2;
            )"));

           {
                auto result = session.ExplainDataQuery(
                    query)
                    .ExtractValueSync();

                UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString().c_str());

                if (UseNewEngine) {
                    UNIT_ASSERT_C(result.GetAst().Contains("'('\"ItemsLimit"), result.GetAst());
                    UNIT_ASSERT_C(result.GetAst().Contains("'('\"Reverse\")"), result.GetAst());
                } else {
                    UNIT_ASSERT_C(result.GetAst().Contains("'\"Reverse\" (Bool '\"true\")"), result.GetAst());
                    UNIT_ASSERT_C(result.GetAst().Contains("Sort"), result.GetAst());
                    UNIT_ASSERT_C(!result.GetAst().Contains("PartialSort"), result.GetAst());
                }
            }

            {
                auto result = session.ExecuteDataQuery(
                    query,
                    TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx(), execSettings)
                    .ExtractValueSync();
                UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString().c_str());

                UNIT_ASSERT_VALUES_EQUAL(NYdb::FormatResultSetYson(result.GetResultSet(0)), "[[[1577836805u];[\"Vasya\"];[5u];[\"Value5\"]];[[1577836804u];[\"Vasya\"];[4u];[\"Value4\"]]]");

                auto& stats = NYdb::TProtoAccessor::GetProto(*result.GetStats());

                int indexPhaseId = 0;

                UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(indexPhaseId).table_access().size(), 1);
                UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(indexPhaseId).table_access(0).name(), "/Root/TestTable/ix_cust3/indexImplTable");
                UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(indexPhaseId).table_access(0).reads().rows(), 2);
            }
        }
    }

    Y_UNIT_TEST_TWIN(SecondaryIndexReplace, UseNewEngine) {
        auto setting = NKikimrKqp::TKqpSetting();
        auto serverSettings = TKikimrSettings()
            .SetEnableMvcc(true)
            .SetEnableMvccSnapshotReads(true)
            .SetKqpSettings({setting});
        TKikimrRunner kikimr(serverSettings);
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        {
            auto tableBuilder = db.GetTableBuilder();
            tableBuilder
                .AddNullableColumn("Key", EPrimitiveType::String)
                .AddNullableColumn("KeyA", EPrimitiveType::Uint64)
                .AddNullableColumn("Index2", EPrimitiveType::String)
                .AddNullableColumn("Index2A", EPrimitiveType::Uint8)
                .AddNullableColumn("Value", EPrimitiveType::Utf8);
            tableBuilder.SetPrimaryKeyColumns(TVector<TString>{"Key", "KeyA"});
            tableBuilder.AddSecondaryIndex("Index", TVector<TString>{"Index2", "Index2A"});
            auto result = session.CreateTable("/Root/TestTable", tableBuilder.Build()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL(result.IsTransportError(), false);
            UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), EStatus::SUCCESS);
        }

        {
            const TString query(Q_(R"(
                REPLACE INTO `/Root/TestTable` (Key, KeyA, Index2, Index2A, Value) VALUES
                    ("Primary1", 41, "Secondary1", 1, "Value1"),
                    ("Primary2", 42, "Secondary2", 2, "Value2"),
                    ("Primary3", 43, "Secondary3", 3, "Value3");
                )"));
            auto result = session.ExecuteDataQuery(
                    query,
                    TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
        }
        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable");
            const TString expected =
R"([[["Primary1"];[41u];["Secondary1"];[1u];["Value1"]];[["Primary2"];[42u];["Secondary2"];[2u];["Value2"]];[["Primary3"];[43u];["Secondary3"];[3u];["Value3"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }

        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index/indexImplTable");
            const TString expected =
R"([[["Secondary1"];[1u];["Primary1"];[41u]];[["Secondary2"];[2u];["Primary2"];[42u]];[["Secondary3"];[3u];["Primary3"];[43u]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }

        {
            const TString query(Q_(R"(
                REPLACE INTO `/Root/TestTable` (Key, KeyA, Value) VALUES
                    ("Primary1", 41, "Value1_1");
                )"));
            auto result = session.ExecuteDataQuery(
                    query,
                    TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
        }

        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable");
            const TString expected =
R"([[["Primary1"];[41u];#;#;["Value1_1"]];[["Primary2"];[42u];["Secondary2"];[2u];["Value2"]];[["Primary3"];[43u];["Secondary3"];[3u];["Value3"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }

        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index/indexImplTable");
            const TString expected =
R"([[#;#;["Primary1"];[41u]];[["Secondary2"];[2u];["Primary2"];[42u]];[["Secondary3"];[3u];["Primary3"];[43u]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }
    }

    Y_UNIT_TEST_TWIN(SecondaryIndexInsert1, UseNewEngine) {
        auto setting = NKikimrKqp::TKqpSetting();
        auto serverSettings = TKikimrSettings()
            .SetEnableMvcc(true)
            .SetEnableMvccSnapshotReads(true)
            .SetKqpSettings({setting});
        TKikimrRunner kikimr(serverSettings);
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        {
            auto tableBuilder = db.GetTableBuilder();
            tableBuilder
                .AddNullableColumn("Key", EPrimitiveType::String)
                .AddNullableColumn("Index2", EPrimitiveType::String)
                .AddNullableColumn("Value", EPrimitiveType::String);
            tableBuilder.SetPrimaryKeyColumns(TVector<TString>{"Key"});
            tableBuilder.AddSecondaryIndex("Index", TVector<TString>{"Index2"});
            auto result = session.CreateTable("/Root/TestTable", tableBuilder.Build()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL(result.IsTransportError(), false);
            UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), EStatus::SUCCESS);
        }

        {
            const TString query(Q_(R"(
                INSERT INTO `/Root/TestTable` (Key, Value) VALUES
                ("Primary1", "Value1"),
                ("Primary2", "Value2"),
                ("Primary3", "Value3");
            )"));

            auto result = session.ExecuteDataQuery(
                query,
                TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                .ExtractValueSync();
            result.GetIssues().PrintTo(Cerr);
            UNIT_ASSERT(result.IsSuccess());
        }

        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index/indexImplTable");
            const TString expected = R"([[#;["Primary1"]];[#;["Primary2"]];[#;["Primary3"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }
    }

    Y_UNIT_TEST_TWIN(MultipleSecondaryIndex, UseNewEngine) {
        auto setting = NKikimrKqp::TKqpSetting();
        auto serverSettings = TKikimrSettings()
            .SetEnableMvcc(true)
            .SetEnableMvccSnapshotReads(true)
            .SetKqpSettings({setting});
        TKikimrRunner kikimr(serverSettings);
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        {
            auto tableBuilder = db.GetTableBuilder();
            tableBuilder
                .AddNullableColumn("Key", EPrimitiveType::String)
                .AddNullableColumn("Value1", EPrimitiveType::String)
                .AddNullableColumn("Value2", EPrimitiveType::String);
            tableBuilder.SetPrimaryKeyColumns(TVector<TString>{"Key"});
            tableBuilder.AddSecondaryIndex("Index1", "Value1");
            tableBuilder.AddSecondaryIndex("Index2", "Value2");
            auto result = session.CreateTable("/Root/TestTable", tableBuilder.Build()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL(result.IsTransportError(), false);
            UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), EStatus::SUCCESS);
        }

        const TString query1(Q_(R"(
            UPSERT INTO `/Root/TestTable` (Key, Value1, Value2) VALUES
            ("Primary1", "Val1", "Val2");
        )"));

        auto explainResult = session.ExplainDataQuery(query1).ExtractValueSync();
        UNIT_ASSERT_C(explainResult.IsSuccess(), explainResult.GetIssues().ToString());

        NJson::TJsonValue plan;
        NJson::ReadJsonTree(explainResult.GetPlan(), &plan, true);

        UNIT_ASSERT(plan.GetMapSafe().contains("tables"));
        const auto& tables = plan.GetMapSafe().at("tables").GetArraySafe();
        UNIT_ASSERT(tables.size() == 3);
        UNIT_ASSERT(tables.at(0).GetMapSafe().at("name").GetStringSafe() == "/Root/TestTable");
        UNIT_ASSERT(tables.at(1).GetMapSafe().at("name").GetStringSafe() == "/Root/TestTable/Index1/indexImplTable");
        UNIT_ASSERT(tables.at(2).GetMapSafe().at("name").GetStringSafe() == "/Root/TestTable/Index2/indexImplTable");

        auto result = session.ExecuteDataQuery(
                query1,
                TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
            .ExtractValueSync();
        UNIT_ASSERT(result.IsSuccess());

        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index1/indexImplTable");
            const TString expected = R"([[["Val1"];["Primary1"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }
        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index2/indexImplTable");
            const TString expected = R"([[["Val2"];["Primary1"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }
        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable");
            const TString expected = R"([[["Primary1"];["Val1"];["Val2"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }
    }

    Y_UNIT_TEST_TWIN(MultipleSecondaryIndexWithSameComulns, UseNewEngine) {
        auto setting = NKikimrKqp::TKqpSetting();
        auto serverSettings = TKikimrSettings()
            .SetEnableMvcc(true)
            .SetEnableMvccSnapshotReads(true)
            .SetKqpSettings({setting});
        TKikimrRunner kikimr(serverSettings);
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        {
            auto tableBuilder = db.GetTableBuilder();
            tableBuilder
                .AddNullableColumn("Key", EPrimitiveType::String)
                .AddNullableColumn("Value1", EPrimitiveType::String)
                .AddNullableColumn("Value2", EPrimitiveType::String)
                .AddNullableColumn("Value3", EPrimitiveType::Int64);
            tableBuilder.SetPrimaryKeyColumns(TVector<TString>{"Key"});
            tableBuilder.AddSecondaryIndex("Index1", TVector<TString>{"Value1", "Value3"});
            tableBuilder.AddSecondaryIndex("Index2", TVector<TString>{"Value2", "Value3"});
            auto result = session.CreateTable("/Root/TestTable", tableBuilder.Build()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL(result.IsTransportError(), false);
            UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), EStatus::SUCCESS);
        }

        {
            const TString query1(Q_(R"(
                UPSERT INTO `/Root/TestTable` (Key, Value1, Value2, Value3) VALUES
                ("Primary1", "Val1", "Val2", 42);
            )"));

            auto result = session.ExecuteDataQuery(
                query1,
                TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
            .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
        }

        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index1/indexImplTable");
            const TString expected = R"([[["Val1"];[42];["Primary1"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }
        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index2/indexImplTable");
            const TString expected = R"([[["Val2"];[42];["Primary1"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }
        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable");
            const TString expected = R"([[["Primary1"];["Val1"];["Val2"];[42]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }

        {
            const TString query(Q1_(R"(
                SELECT Value2 FROM `/Root/TestTable` VIEW Index1 WHERE Value1 = 'Val1';
            )"));

            auto result = session.ExecuteDataQuery(
                query,
                TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
            UNIT_ASSERT_VALUES_EQUAL(NYdb::FormatResultSetYson(result.GetResultSet(0)), "[[[\"Val2\"]]]");
        }

        {
            const TString query1(Q_(R"(
                REPLACE INTO `/Root/TestTable` (Key, Value1, Value2, Value3) VALUES
                ("Primary1", "Val1_1", "Val2_1", 43);
            )"));

            auto result = session.ExecuteDataQuery(
                query1,
                TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
            .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
        }

        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index1/indexImplTable");
            const TString expected = R"([[["Val1_1"];[43];["Primary1"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }
        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index2/indexImplTable");
            const TString expected = R"([[["Val2_1"];[43];["Primary1"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }
        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable");
            const TString expected = R"([[["Primary1"];["Val1_1"];["Val2_1"];[43]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }
        {
            const TString query1(Q_(R"(
                REPLACE INTO `/Root/TestTable` (Key, Value1, Value2) VALUES
                ("Primary1", "Val1_1", "Val2_1");
            )"));

            auto result = session.ExecuteDataQuery(
                query1,
                TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
            .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
        }

        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index1/indexImplTable");
            const TString expected = R"([[["Val1_1"];#;["Primary1"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }
        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index2/indexImplTable");
            const TString expected = R"([[["Val2_1"];#;["Primary1"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }
        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable");
            const TString expected = R"([[["Primary1"];["Val1_1"];["Val2_1"];#]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }
        {
            const TString query1(Q_(R"(
                UPDATE `/Root/TestTable` SET Value3 = 35;
            )"));

            auto result = session.ExecuteDataQuery(
                query1,
                TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
            .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
        }

        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index1/indexImplTable");
            const TString expected = R"([[["Val1_1"];[35];["Primary1"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }
        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index2/indexImplTable");
            const TString expected = R"([[["Val2_1"];[35];["Primary1"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }
        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable");
            const TString expected = R"([[["Primary1"];["Val1_1"];["Val2_1"];[35]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }
        {
            const TString query1(Q_(R"(
                UPDATE `/Root/TestTable` ON (Key, Value3) VALUES ('Primary1', 36);
            )"));

            auto result = session.ExecuteDataQuery(
                query1,
                TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
            .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
        }

        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index1/indexImplTable");
            const TString expected = R"([[["Val1_1"];[36];["Primary1"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }
        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index2/indexImplTable");
            const TString expected = R"([[["Val2_1"];[36];["Primary1"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }
        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable");
            const TString expected = R"([[["Primary1"];["Val1_1"];["Val2_1"];[36]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }
        {
            const TString query1(Q_(R"(
                UPDATE `/Root/TestTable` ON (Key, Value1) VALUES ('Primary1', 'Val1_2');
            )"));

            auto result = session.ExecuteDataQuery(
                query1,
                TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
            .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
        }

        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index1/indexImplTable");
            const TString expected = R"([[["Val1_2"];[36];["Primary1"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }
        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index2/indexImplTable");
            const TString expected = R"([[["Val2_1"];[36];["Primary1"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }
        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable");
            const TString expected = R"([[["Primary1"];["Val1_2"];["Val2_1"];[36]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }
        {
            const TString query1(Q_(R"(
                INSERT INTO `/Root/TestTable` (Key, Value2) VALUES ('Primary2', 'Record2');
            )"));

            auto result = session.ExecuteDataQuery(
                query1,
                TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
            .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
        }

        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index1/indexImplTable");
            const TString expected = R"([[#;#;["Primary2"]];[["Val1_2"];[36];["Primary1"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }
        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index2/indexImplTable");
            const TString expected = R"([[["Record2"];#;["Primary2"]];[["Val2_1"];[36];["Primary1"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }
        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable");
            const TString expected = R"([[["Primary1"];["Val1_2"];["Val2_1"];[36]];[["Primary2"];#;["Record2"];#]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }
        {
            const TString query1(Q_(R"(
                INSERT INTO `/Root/TestTable` (Key, Value3) VALUES ('Primary3', 37);
            )"));

            auto result = session.ExecuteDataQuery(
                query1,
                TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
            .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
        }

        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index1/indexImplTable");
            const TString expected = R"([[#;#;["Primary2"]];[#;[37];["Primary3"]];[["Val1_2"];[36];["Primary1"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }
        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index2/indexImplTable");
            const TString expected = R"([[#;[37];["Primary3"]];[["Record2"];#;["Primary2"]];[["Val2_1"];[36];["Primary1"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }
        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable");
            const TString expected = R"([[["Primary1"];["Val1_2"];["Val2_1"];[36]];[["Primary2"];#;["Record2"];#];[["Primary3"];#;#;[37]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }
        {
            const TString query1(Q_(R"(
                DELETE FROM `/Root/TestTable` WHERE Key = 'Primary3';
            )"));

            auto result = session.ExecuteDataQuery(
                query1,
                TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
            .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
        }

        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index1/indexImplTable");
            const TString expected = R"([[#;#;["Primary2"]];[["Val1_2"];[36];["Primary1"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }
        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index2/indexImplTable");
            const TString expected = R"([[["Record2"];#;["Primary2"]];[["Val2_1"];[36];["Primary1"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }
        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable");
            const TString expected = R"([[["Primary1"];["Val1_2"];["Val2_1"];[36]];[["Primary2"];#;["Record2"];#]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }
        {
            const TString query1(Q_(R"(
                DELETE FROM `/Root/TestTable` ON (Key) VALUES ('Primary2');
            )"));

            auto result = session.ExecuteDataQuery(
                query1,
                TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
            .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
        }

        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index1/indexImplTable");
            const TString expected = R"([[["Val1_2"];[36];["Primary1"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }
        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index2/indexImplTable");
            const TString expected = R"([[["Val2_1"];[36];["Primary1"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }
        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable");
            const TString expected = R"([[["Primary1"];["Val1_2"];["Val2_1"];[36]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }

        {
            const TString query1(Q_(R"(
                DELETE FROM `/Root/TestTable`;
            )"));

            auto result = session.ExecuteDataQuery(
                query1,
                TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
            .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
        }
        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index1/indexImplTable");
            UNIT_ASSERT_VALUES_EQUAL(yson, "");
        }
        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index2/indexImplTable");
            UNIT_ASSERT_VALUES_EQUAL(yson, "");
        }
        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable");
            UNIT_ASSERT_VALUES_EQUAL(yson, "");
        }
    }

    Y_UNIT_TEST_TWIN(SecondaryIndexWithPrimaryKeySameComulns, UseNewEngine) {
        auto setting = NKikimrKqp::TKqpSetting();
        auto serverSettings = TKikimrSettings()
            .SetEnableMvcc(true)
            .SetEnableMvccSnapshotReads(true)
            .SetKqpSettings({setting});
        TKikimrRunner kikimr(serverSettings);
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        {
            auto tableBuilder = db.GetTableBuilder();
            tableBuilder
                .AddNullableColumn("Key", EPrimitiveType::String)
                .AddNullableColumn("KeyA", EPrimitiveType::Int64)
                .AddNullableColumn("Value1", EPrimitiveType::String)
                .AddNullableColumn("Payload", EPrimitiveType::Utf8);
            tableBuilder.SetPrimaryKeyColumns(TVector<TString>{"Key", "KeyA"});
            tableBuilder.AddSecondaryIndex("Index1", TVector<TString>{"Value1", "KeyA"});
            tableBuilder.AddSecondaryIndex("Index2", TVector<TString>{"Key", "Value1"});
            auto result = session.CreateTable("/Root/TestTable", tableBuilder.Build()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL(result.IsTransportError(), false);
            UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), EStatus::SUCCESS);
        }

        {
            const TString query1(Q_(R"(
                UPSERT INTO `/Root/TestTable` (Key, KeyA, Value1, Payload) VALUES
                ("Primary1", 42, "Val1", "SomeData");
            )"));

            auto result = session.ExecuteDataQuery(
                query1,
                TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
            .ExtractValueSync();
            UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());
        }
        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index1/indexImplTable");
            const TString expected = R"([[["Val1"];[42];["Primary1"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }

        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index2/indexImplTable");
            const TString expected = R"([[["Primary1"];["Val1"];[42]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }

        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable");
            const TString expected = R"([[["Primary1"];[42];["Val1"];["SomeData"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }

        {
            const TString query(Q1_(R"(
                SELECT Key FROM `/Root/TestTable` VIEW Index1 WHERE Value1 = 'Val1';
            )"));

            auto result = session.ExecuteDataQuery(
                query,
                TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
            UNIT_ASSERT_VALUES_EQUAL(NYdb::FormatResultSetYson(result.GetResultSet(0)), "[[[\"Primary1\"]]]");
        }

        {
            const TString query(Q1_(R"(
                SELECT Key, Value1, Payload FROM `/Root/TestTable` VIEW Index2 WHERE Key = 'Primary1' ORDER BY Key, Value1;
            )"));

            auto result = session.ExecuteDataQuery(
                query,
                TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
            UNIT_ASSERT_VALUES_EQUAL(NYdb::FormatResultSetYson(result.GetResultSet(0)), "[[[\"Primary1\"];[\"Val1\"];[\"SomeData\"]]]");
        }

        {
            const TString query1(Q_(R"(
                UPSERT INTO `/Root/TestTable` (Key, KeyA, Value1, Payload) VALUES
                ("Primary1", 42, "Val1_0", "SomeData2");
            )"));

            auto result = session.ExecuteDataQuery(
                query1,
                TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
            .ExtractValueSync();
            UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());
        }

        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index1/indexImplTable");
            const TString expected = R"([[["Val1_0"];[42];["Primary1"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }

        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index2/indexImplTable");
            const TString expected = R"([[["Primary1"];["Val1_0"];[42]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }

        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable");
            const TString expected = R"([[["Primary1"];[42];["Val1_0"];["SomeData2"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }

        {
            const TString query1(Q_(R"(
                REPLACE INTO `/Root/TestTable` (Key, KeyA, Value1) VALUES
                ("Primary1", 42, "Val1_1");
            )"));

            auto result = session.ExecuteDataQuery(
                query1,
                TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
            .ExtractValueSync();
            UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());
        }
        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index1/indexImplTable");
            const TString expected = R"([[["Val1_1"];[42];["Primary1"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }

        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index2/indexImplTable");
            const TString expected = R"([[["Primary1"];["Val1_1"];[42]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }

        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable");
            const TString expected = R"([[["Primary1"];[42];["Val1_1"];#]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }

        {
            const TString query1(Q_(R"(
                REPLACE INTO `/Root/TestTable` (Key, KeyA) VALUES
                ("Primary1", 42);
            )"));

            auto result = session.ExecuteDataQuery(
                query1,
                TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
            .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
        }
        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index1/indexImplTable");
            const TString expected = R"([[#;[42];["Primary1"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }
        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable");
            const TString expected = R"([[["Primary1"];[42];#;#]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }

        {
            const TString query1(Q_(R"(
                UPDATE `/Root/TestTable` SET Value1 = 'Val1_2';
            )"));

            auto result = session.ExecuteDataQuery(
                query1,
                TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
            .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
        }
        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index1/indexImplTable");
            const TString expected = R"([[["Val1_2"];[42];["Primary1"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }
        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable");
            const TString expected = R"([[["Primary1"];[42];["Val1_2"];#]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }

        {
            const TString query1(Q_(R"(
                UPDATE `/Root/TestTable` ON (Key, KeyA, Value1) VALUES ('Primary1', 42, 'Val1_3');
            )"));

            auto result = session.ExecuteDataQuery(
                query1,
                TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
            .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
        }
        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index1/indexImplTable");
            const TString expected = R"([[["Val1_3"];[42];["Primary1"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }
        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable");
            const TString expected = R"([[["Primary1"];[42];["Val1_3"];#]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }

        {
            const TString query1(Q_(R"(
                INSERT INTO `/Root/TestTable` (Key, KeyA, Value1) VALUES ('Primary2', 43, 'Val2');
            )"));

            auto result = session.ExecuteDataQuery(
                query1,
                TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
            .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
        }
        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index1/indexImplTable");
            const TString expected = R"([[["Val1_3"];[42];["Primary1"]];[["Val2"];[43];["Primary2"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }
        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable");
            const TString expected = R"([[["Primary1"];[42];["Val1_3"];#];[["Primary2"];[43];["Val2"];#]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }

        {
            const TString query1(Q_(R"(
                DELETE FROM `/Root/TestTable` WHERE Key = 'Primary2';
            )"));

            auto result = session.ExecuteDataQuery(
                query1,
                TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
            .ExtractValueSync();
            UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());
        }
        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index1/indexImplTable");
            const TString expected = R"([[["Val1_3"];[42];["Primary1"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }
        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable");
            const TString expected = R"([[["Primary1"];[42];["Val1_3"];#]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }

        {
            const TString query1(Q_(R"(
                DELETE FROM `/Root/TestTable` ON (Key, KeyA) VALUES ('Primary1', 42);
            )"));

            auto result = session.ExecuteDataQuery(
                query1,
                TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
            .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
        }
        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index1/indexImplTable");
            UNIT_ASSERT_VALUES_EQUAL(yson, "");
        }
        {
            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable");
            UNIT_ASSERT_VALUES_EQUAL(yson, "");
        }
    }

    Y_UNIT_TEST(DeleteOnWithSubquery) {
        auto setting = NKikimrKqp::TKqpSetting();
        auto serverSettings = TKikimrSettings()
            .SetEnableMvcc(true)
            .SetEnableMvccSnapshotReads(true)
            .SetKqpSettings({setting});
        TKikimrRunner kikimr(serverSettings);
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();
        CreateSampleTablesWithIndex(session);

        const TString query(R"(
            --!syntax_v1
            PRAGMA Kikimr.UseNewEngine = 'false';
            DECLARE $keys AS List<Tuple<Int32, String>>;
            $to_delete = (
                SELECT Key FROM `/Root/SecondaryComplexKeys` VIEW Index WHERE (Fk1, Fk2) in $keys
            );
            DELETE FROM `/Root/SecondaryComplexKeys` ON
            SELECT * FROM $to_delete;
        )");

        auto params = TParamsBuilder().AddParam("$keys").BeginList()
            .AddListItem().BeginTuple().AddElement().Int32(1).AddElement().String("Fk1").EndTuple()
            .EndList().Build().Build();

        NYdb::NTable::TExecDataQuerySettings execSettings;
        execSettings.CollectQueryStats(ECollectQueryStatsMode::Basic);
        auto result = session.ExecuteDataQuery(query, TTxControl::BeginTx().CommitTx(), params, execSettings).ExtractValueSync();
        UNIT_ASSERT(result.IsSuccess());

        const auto& stats = NYdb::TProtoAccessor::GetProto(*result.GetStats());

        UNIT_ASSERT_VALUES_EQUAL(stats.query_phases().size(), 4);

        UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(1).table_access().size(), 1);
        UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(1).table_access(0).name(), "/Root/SecondaryComplexKeys/Index/indexImplTable");
        UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(1).table_access(0).reads().rows(), 1);
        // In theory we can optimize and remove this read access
        UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(2).table_access().size(), 1);
        UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(2).table_access(0).name(), "/Root/SecondaryComplexKeys");
        UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(2).table_access(0).reads().rows(), 1);
        UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(3).table_access().size(), 2);

        // No guarantee effects will be in the same order
        if (stats.query_phases(3).table_access(0).name() == "/Root/SecondaryComplexKeys") {
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(3).table_access(1).name(), "/Root/SecondaryComplexKeys/Index/indexImplTable");
        } else if (stats.query_phases(3).table_access(0).name() == "/Root/SecondaryComplexKeys/Index/indexImplTable") {
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(3).table_access(1).name(), "/Root/SecondaryComplexKeys");
        } else {
            Y_FAIL("unexpected table name");
        }
        UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(3).table_access(0).deletes().rows(), 1);
        UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(3).table_access(1).deletes().rows(), 1);
    }

    Y_UNIT_TEST_TWIN(SecondaryIndexUsingInJoin, UseNewEngine) {
        auto setting = NKikimrKqp::TKqpSetting();
        auto serverSettings = TKikimrSettings()
            .SetEnableMvcc(true)
            .SetEnableMvccSnapshotReads(true)
            .SetKqpSettings({setting});
        TKikimrRunner kikimr(serverSettings);
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        {
            auto tableBuilder = db.GetTableBuilder();
            tableBuilder
                .AddNullableColumn("Key", EPrimitiveType::String)
                .AddNullableColumn("Value", EPrimitiveType::Int64);
            tableBuilder.SetPrimaryKeyColumns(TVector<TString>{"Key"});
            tableBuilder.AddSecondaryIndex("Index1", "Value");
            auto result = session.CreateTable("/Root/TestTable1", tableBuilder.Build()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL(result.IsTransportError(), false);
            UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), EStatus::SUCCESS);
        }
        {
            auto tableBuilder = db.GetTableBuilder();
            tableBuilder
                .AddNullableColumn("Key", EPrimitiveType::String)
                .AddNullableColumn("Value", EPrimitiveType::Int64);
            tableBuilder.SetPrimaryKeyColumns(TVector<TString>{"Key"});
            tableBuilder.AddSecondaryIndex("Index1", "Value");
            auto result = session.CreateTable("/Root/TestTable2", tableBuilder.Build()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL(result.IsTransportError(), false);
            UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), EStatus::SUCCESS);
        }
        {
            const TString query1(Q_(R"(
                UPSERT INTO `/Root/TestTable1` (Key, Value) VALUES
                    ("Table1Primary3", 3),
                    ("Table1Primary4", 4),
                    ("Table1Primary55", 55);

                UPSERT INTO `/Root/TestTable2` (Key, Value) VALUES
                    ("Table2Primary1", 1),
                    ("Table2Primary2", 2),
                    ("Table2Primary3", 3),
                    ("Table2Primary4", 4),
                    ("Table2Primary5", 5);
            )"));

            auto result = session.ExecuteDataQuery(
                query1,
                TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
            .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
        }

        NYdb::NTable::TExecDataQuerySettings execSettings;
        execSettings.CollectQueryStats(ECollectQueryStatsMode::Basic);

        {
            const TString query(Q1_(R"(
                SELECT `/Root/TestTable1`.Key FROM `/Root/TestTable1`
                    INNER JOIN `/Root/TestTable2` VIEW Index1 as t2 ON t2.Value = `/Root/TestTable1`.Value;
            )"));

            auto result = session.ExecuteDataQuery(
                query,
                TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx(),
                execSettings)
                .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
            UNIT_ASSERT(result.GetIssues().Empty());
            UNIT_ASSERT_VALUES_EQUAL(NYdb::FormatResultSetYson(result.GetResultSet(0)),
                "[[[\"Table1Primary3\"]];[[\"Table1Primary4\"]]]");

            auto& stats = NYdb::TProtoAccessor::GetProto(*result.GetStats());

            int indexPhaseId = 1;
            if (UseNewEngine) {
                UNIT_ASSERT_VALUES_EQUAL(stats.query_phases().size(), 3);
                indexPhaseId = 2;
            }

            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(0).table_access().size(), 1);
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(0).table_access(0).name(), "/Root/TestTable1");
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(0).table_access(0).reads().rows(), 3);

            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(indexPhaseId).table_access().size(), 1);
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(indexPhaseId).table_access(0).name(), "/Root/TestTable2/Index1/indexImplTable");
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(indexPhaseId).table_access(0).reads().rows(), 2);
        }

        {
            const TString query(Q1_(R"(
                SELECT `/Root/TestTable1`.Key, `/Root/TestTable1`.Value FROM `/Root/TestTable1`
                    INNER JOIN `/Root/TestTable2` VIEW Index1 as t2 ON t2.Value = `/Root/TestTable1`.Value ORDER BY `/Root/TestTable1`.Value DESC;
            )"));

            auto result = session.ExecuteDataQuery(
                query,
                TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx(),
                execSettings)
                .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
            UNIT_ASSERT(result.GetIssues().Empty());
            UNIT_ASSERT_VALUES_EQUAL(NYdb::FormatResultSetYson(result.GetResultSet(0)),
                "[[[\"Table1Primary4\"];[4]];[[\"Table1Primary3\"];[3]]]");

            auto& stats = NYdb::TProtoAccessor::GetProto(*result.GetStats());

            int indexPhaseId = 1;
            if (UseNewEngine) {
                UNIT_ASSERT_VALUES_EQUAL(stats.query_phases().size(), 3);
                indexPhaseId = 2;
            }

            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(0).table_access().size(), 1);
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(0).table_access(0).name(), "/Root/TestTable1");
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(0).table_access(0).reads().rows(), 3);

            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(indexPhaseId).table_access().size(), 1);
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(indexPhaseId).table_access(0).name(), "/Root/TestTable2/Index1/indexImplTable");
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(indexPhaseId).table_access(0).reads().rows(), 2);
        }

        {
            const TString query(Q1_(R"(
                SELECT `/Root/TestTable1`.Key FROM `/Root/TestTable1`
                    LEFT JOIN `/Root/TestTable2` VIEW Index1 as t2 ON t2.Value = `/Root/TestTable1`.Value;
            )"));

            auto result = session.ExecuteDataQuery(
                query,
                TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx(),
                execSettings)
                .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
            UNIT_ASSERT(result.GetIssues().Empty());
            UNIT_ASSERT_VALUES_EQUAL(NYdb::FormatResultSetYson(result.GetResultSet(0)),
                "[[[\"Table1Primary3\"]];[[\"Table1Primary4\"]];[[\"Table1Primary55\"]]]");

            auto& stats = NYdb::TProtoAccessor::GetProto(*result.GetStats());

            int indexPhaseId = 1;
            if (UseNewEngine) {
                UNIT_ASSERT_VALUES_EQUAL(stats.query_phases().size(), 3);
                indexPhaseId = 2;
            }

            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(0).table_access().size(), 1);
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(0).table_access(0).name(), "/Root/TestTable1");
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(0).table_access(0).reads().rows(), 3);

            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(indexPhaseId).table_access().size(), 1);
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(indexPhaseId).table_access(0).name(), "/Root/TestTable2/Index1/indexImplTable");
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(indexPhaseId).table_access(0).reads().rows(), 2);
        }

        {
            const TString query(Q1_(R"(
                SELECT `/Root/TestTable1`.Key, `/Root/TestTable1`.Value FROM `/Root/TestTable1`
                    LEFT JOIN `/Root/TestTable2` VIEW Index1 as t2 ON t2.Value = `/Root/TestTable1`.Value ORDER BY `/Root/TestTable1`.Value DESC;
            )"));

            auto result = session.ExecuteDataQuery(
                query,
                TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx(),
                execSettings)
                .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
            UNIT_ASSERT(result.GetIssues().Empty());
            UNIT_ASSERT_VALUES_EQUAL(NYdb::FormatResultSetYson(result.GetResultSet(0)),
                "[[[\"Table1Primary55\"];[55]];[[\"Table1Primary4\"];[4]];[[\"Table1Primary3\"];[3]]]");

            auto& stats = NYdb::TProtoAccessor::GetProto(*result.GetStats());

            int indexPhaseId = 1;
            if (UseNewEngine) {
                UNIT_ASSERT_VALUES_EQUAL(stats.query_phases().size(), 3);
                indexPhaseId = 2;
            }

            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(0).table_access().size(), 1);
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(0).table_access(0).name(), "/Root/TestTable1");
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(0).table_access(0).reads().rows(), 3);

            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(indexPhaseId).table_access().size(), 1);
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(indexPhaseId).table_access(0).name(), "/Root/TestTable2/Index1/indexImplTable");
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(indexPhaseId).table_access(0).reads().rows(), 2);
        }
    }

    Y_UNIT_TEST_TWIN(SecondaryIndexUsingInJoin2, UseNewEngine) {
        auto setting = NKikimrKqp::TKqpSetting();
        auto serverSettings = TKikimrSettings()
            .SetEnableMvcc(true)
            .SetEnableMvccSnapshotReads(true)
            .SetKqpSettings({setting});
        TKikimrRunner kikimr(serverSettings);
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        NYdb::NTable::TExecDataQuerySettings execSettings;
        execSettings.CollectQueryStats(ECollectQueryStatsMode::Basic);


        {
            auto tableBuilder = db.GetTableBuilder();
            tableBuilder
                .AddNullableColumn("Key", EPrimitiveType::String)
                .AddNullableColumn("Value", EPrimitiveType::Int64);
            tableBuilder.SetPrimaryKeyColumns(TVector<TString>{"Key"});
            tableBuilder.AddSecondaryIndex("Index1", "Value");
            auto result = session.CreateTable("/Root/TestTable1", tableBuilder.Build()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL(result.IsTransportError(), false);
            UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), EStatus::SUCCESS);
        }
        {
            auto tableBuilder = db.GetTableBuilder();
            tableBuilder
                .AddNullableColumn("Key", EPrimitiveType::String)
                .AddNullableColumn("Value", EPrimitiveType::Int64)
                .AddNullableColumn("Value2", EPrimitiveType::String);
            tableBuilder.SetPrimaryKeyColumns(TVector<TString>{"Key"});
            tableBuilder.AddSecondaryIndex("Index1", "Value");
            auto result = session.CreateTable("/Root/TestTable2", tableBuilder.Build()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL(result.IsTransportError(), false);
            UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), EStatus::SUCCESS);
        }
        {
            const TString query1(Q_(R"(
                UPSERT INTO `/Root/TestTable1` (Key, Value) VALUES
                    ("Table1Primary3", 3),
                    ("Table1Primary4", 4),
                    ("Table1Primary55", 55);

                UPSERT INTO `/Root/TestTable2` (Key, Value, Value2) VALUES
                    ("Table2Primary1", 1, "aa"),
                    ("Table2Primary2", 2, "bb"),
                    ("Table2Primary3", 3, "cc"),
                    ("Table2Primary4", 4, "dd"),
                    ("Table2Primary5", 5, "ee");
            )"));

            auto result = session.ExecuteDataQuery(
                query1,
                TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
            .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
        }
        {
            const TString query(Q1_(R"(
                SELECT t1.Key, t2.Value2 FROM `/Root/TestTable1` as t1
                    INNER JOIN `/Root/TestTable2` VIEW Index1 as t2 ON t1.Value = t2.Value;
            )"));

            auto result = session.ExecuteDataQuery(
                query,
                TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx(),
                execSettings)
                .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
            UNIT_ASSERT(result.GetIssues().Empty());
            UNIT_ASSERT_VALUES_EQUAL(NYdb::FormatResultSetYson(result.GetResultSet(0)),
                "[[[\"Table1Primary3\"];[\"cc\"]];[[\"Table1Primary4\"];[\"dd\"]]]");

            auto& stats = NYdb::TProtoAccessor::GetProto(*result.GetStats());

            int indexPhaseId = 1;
            if (UseNewEngine) {
                UNIT_ASSERT_VALUES_EQUAL(stats.query_phases().size(), 4);
                indexPhaseId = 2;
            }

            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(0).table_access().size(), 1);
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(0).table_access(0).name(), "/Root/TestTable1");
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(0).table_access(0).reads().rows(), 3);

            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(indexPhaseId).table_access().size(), 1);
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(indexPhaseId).table_access(0).name(), "/Root/TestTable2/Index1/indexImplTable");
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(indexPhaseId).table_access(0).reads().rows(), 2);

            indexPhaseId++;

            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(indexPhaseId).table_access().size(), 1);
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(indexPhaseId).table_access(0).name(), "/Root/TestTable2");
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(indexPhaseId).table_access(0).reads().rows(), 2);
        }

        {
            const TString query(Q1_(R"(
                SELECT t1.Key, t2.Value2 FROM `/Root/TestTable1` as t1
                    LEFT JOIN `/Root/TestTable2` VIEW Index1 as t2 ON t1.Value = t2.Value;
            )"));

            auto result = session.ExecuteDataQuery(
                query,
                TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx(),
                execSettings)
                .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
            UNIT_ASSERT(result.GetIssues().Empty());
            UNIT_ASSERT_VALUES_EQUAL(NYdb::FormatResultSetYson(result.GetResultSet(0)),
                "[[[\"Table1Primary3\"];[\"cc\"]];[[\"Table1Primary4\"];[\"dd\"]];[[\"Table1Primary55\"];#]]");

            auto& stats = NYdb::TProtoAccessor::GetProto(*result.GetStats());

            int indexPhaseId = 1;
            if (UseNewEngine) {
                UNIT_ASSERT_VALUES_EQUAL(stats.query_phases().size(), 4);
                indexPhaseId = 2;
            }

            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(0).table_access().size(), 1);
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(0).table_access(0).name(), "/Root/TestTable1");
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(0).table_access(0).reads().rows(), 3);

            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(indexPhaseId).table_access().size(), 1);
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(indexPhaseId).table_access(0).name(), "/Root/TestTable2/Index1/indexImplTable");
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(indexPhaseId).table_access(0).reads().rows(), 2);

            indexPhaseId++;

            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(indexPhaseId).table_access().size(), 1);
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(indexPhaseId).table_access(0).name(), "/Root/TestTable2");
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(indexPhaseId).table_access(0).reads().rows(), 2);
        }
    }

    Y_UNIT_TEST_TWIN(ForbidViewModification, UseNewEngine) {
        auto setting = NKikimrKqp::TKqpSetting();
        auto serverSettings = TKikimrSettings()
            .SetEnableMvcc(true)
            .SetEnableMvccSnapshotReads(true)
            .SetKqpSettings({setting});
        TKikimrRunner kikimr(serverSettings);
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        {
            auto tableBuilder = db.GetTableBuilder();
            tableBuilder
                .AddNullableColumn("Key", EPrimitiveType::String)
                .AddNullableColumn("Index2", EPrimitiveType::String)
                .AddNullableColumn("Value", EPrimitiveType::String);
            tableBuilder.SetPrimaryKeyColumns(TVector<TString>{"Key"});
            tableBuilder.AddSecondaryIndex("Index", TVector<TString>{"Index2"});
            auto result = session.CreateTable("/Root/TestTable", tableBuilder.Build()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL(result.IsTransportError(), false);
            UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), EStatus::SUCCESS);

            const TString query1(Q_(R"(
                UPSERT INTO `/Root/TestTable` (Key, Index2, Value) VALUES
                ("Primary1", "Secondary1", "Value1");
            )"));

            auto result1 = session.ExecuteDataQuery(
                                     query1,
                                     TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                              .ExtractValueSync();
            UNIT_ASSERT(result1.IsSuccess());
        }

        {
            const TString query(Q1_(R"(
                INSERT INTO `/Root/TestTable` VIEW Index (Index2, Key) VALUES('Secondary2', 'Primary2');
            )"));

            auto result = session.ExecuteDataQuery(
                                 query,
                                 TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                          .ExtractValueSync();
            UNIT_ASSERT(result.GetIssues().ToString().Contains("Unexpected token"));
            UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), NYdb::EStatus::GENERIC_ERROR);
        }

        {
            const TString query(Q1_(R"(
                UPSERT INTO `/Root/TestTable` VIEW Index (Index2, Key) VALUES('Secondary2', 'Primary2');
            )"));

            auto result = session.ExecuteDataQuery(
                                 query,
                                 TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                          .ExtractValueSync();
            UNIT_ASSERT(result.GetIssues().ToString().Contains("Unexpected token"));
            UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), NYdb::EStatus::GENERIC_ERROR);
        }

        {
            const TString query(Q1_(R"(
                UPDATE `/Root/TestTable` VIEW Index ON (Index2, Key) VALUES ('Primary1', 'Secondary1');
            )"));

            auto result = session.ExecuteDataQuery(
                                     query,
                                     TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                              .ExtractValueSync();
            UNIT_ASSERT(result.GetIssues().ToString().Contains("Unexpected token"));
            UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), NYdb::EStatus::GENERIC_ERROR);
        }

        {
            const TString query(Q1_(R"(
                DELETE FROM `/Root/TestTable` VIEW Index WHERE Index2 = 'Secondary1';
            )"));

            auto result = session.ExecuteDataQuery(
                                     query,
                                     TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                              .ExtractValueSync();
            UNIT_ASSERT(result.GetIssues().ToString().Contains("Unexpected token"));
            UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), NYdb::EStatus::GENERIC_ERROR);
        }

    }

    Y_UNIT_TEST(ForbidDirectIndexTableCreation) {
        auto setting = NKikimrKqp::TKqpSetting();
        auto serverSettings = TKikimrSettings()
            .SetEnableMvcc(true)
            .SetEnableMvccSnapshotReads(true)
            .SetKqpSettings({setting});
        TKikimrRunner kikimr(serverSettings);
        auto db = kikimr.GetTableClient();
        auto scheme = kikimr.GetSchemeClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        {
            auto tableBuilder = db.GetTableBuilder();
            tableBuilder
                .AddNullableColumn("Key", EPrimitiveType::String)
                .AddNullableColumn("Value", EPrimitiveType::String);
            tableBuilder.SetPrimaryKeyColumns(TVector<TString>{"Key"});
            auto result = session.CreateTable("/Root/TestTable", tableBuilder.Build()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL(result.IsTransportError(), false);
            UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), EStatus::SUCCESS);
        }

        {
            auto tableBuilder = db.GetTableBuilder();
            tableBuilder
                .AddNullableColumn("Value", EPrimitiveType::String)
                .AddNullableColumn("Key", EPrimitiveType::String);
            tableBuilder.SetPrimaryKeyColumns(TVector<TString>{"Value"});
            auto result = session.CreateTable("/Root/TestTable/Index", tableBuilder.Build()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL(result.IsTransportError(), false);
            UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), EStatus::GENERIC_ERROR);
        }

        {
            auto result = scheme.MakeDirectory("/Root/TestTable/Index").ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL(result.IsTransportError(), false);
            UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), EStatus::GENERIC_ERROR);
        }

        {
            auto tableBuilder = db.GetTableBuilder();
            tableBuilder
                .AddNullableColumn("Value", EPrimitiveType::String)
                .AddNullableColumn("Key", EPrimitiveType::String);
            tableBuilder.SetPrimaryKeyColumns(TVector<TString>{"Value"});
            auto result = session.CreateTable("/Root/TestTable/Index/indexImplTable", tableBuilder.Build()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL(result.IsTransportError(), false);
            UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), EStatus::GENERIC_ERROR);
        }
    }

    Y_UNIT_TEST_TWIN(DuplicateUpsertInterleave, UseNewEngine) {
        auto setting = NKikimrKqp::TKqpSetting();
        auto serverSettings = TKikimrSettings()
            .SetEnableMvcc(true)
            .SetEnableMvccSnapshotReads(true)
            .SetKqpSettings({setting});
        TKikimrRunner kikimr(serverSettings);
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        auto tableBuilder = db.GetTableBuilder();
        tableBuilder
            .AddNullableColumn("Key", EPrimitiveType::String)
            .AddNullableColumn("Index2", EPrimitiveType::String)
            .AddNullableColumn("Value", EPrimitiveType::String);
        tableBuilder.SetPrimaryKeyColumns(TVector<TString>{"Key"});
        tableBuilder.AddSecondaryIndex("Index", TVector<TString>{"Index2"});
        auto result = session.CreateTable("/Root/TestTable", tableBuilder.Build()).ExtractValueSync();
        UNIT_ASSERT_VALUES_EQUAL(result.IsTransportError(), false);
        UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), EStatus::SUCCESS);

        {
            const TString query(Q_(R"(
                UPSERT INTO `/Root/TestTable` (Key, Index2, Value) VALUES
                    ("Primary1", "Secondary1", "Value1"),
                    ("Primary2", "Secondary2", "Value2"),
                    ("Primary1", "Secondary11", "Value3"),
                    ("Primary2", "Secondary22", "Value3");
            )"));

            auto result = session.ExecuteDataQuery(query, TTxControl::BeginTx().CommitTx()).ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());

            CompareYson(R"([[["Secondary11"];["Primary1"]];[["Secondary22"];["Primary2"]]])",
                ReadTablePartToYson(session, "/Root/TestTable/Index/indexImplTable"));
        }
    }

    Y_UNIT_TEST_TWIN(DuplicateUpsertInterleaveParams, UseNewEngine) {
        auto setting = NKikimrKqp::TKqpSetting();
        auto serverSettings = TKikimrSettings()
            .SetEnableMvcc(true)
            .SetEnableMvccSnapshotReads(true)
            .SetKqpSettings({setting});
        TKikimrRunner kikimr(serverSettings);
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        auto tableBuilder = db.GetTableBuilder();
        tableBuilder
            .AddNullableColumn("Key", EPrimitiveType::String)
            .AddNullableColumn("Index2", EPrimitiveType::String)
            .AddNullableColumn("Value", EPrimitiveType::String);
        tableBuilder.SetPrimaryKeyColumns(TVector<TString>{"Key"});
        tableBuilder.AddSecondaryIndex("Index", TVector<TString>{"Index2"});
        auto result = session.CreateTable("/Root/TestTable", tableBuilder.Build()).ExtractValueSync();
        UNIT_ASSERT_VALUES_EQUAL(result.IsTransportError(), false);
        UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), EStatus::SUCCESS);

        {
            const TString query(Q1_(R"(
                DECLARE $rows AS
                    List<Struct<
                        Key: String?,
                        Index2: String?,
                        Value: String?>>;

                    UPSERT INTO `/Root/TestTable`
                    SELECT Key, Index2, Value FROM AS_TABLE($rows);
            )"));

            auto explainResult = session.ExplainDataQuery(query).ExtractValueSync();
            UNIT_ASSERT_C(explainResult.IsSuccess(), explainResult.GetIssues().ToString());

            NJson::TJsonValue plan;
            NJson::ReadJsonTree(explainResult.GetPlan(), &plan, true);

            UNIT_ASSERT(plan.GetMapSafe().contains("tables"));
            const auto& tables = plan.GetMapSafe().at("tables").GetArraySafe();
            UNIT_ASSERT(tables.size() == 2);
            UNIT_ASSERT(tables.at(0).GetMapSafe().at("name").GetStringSafe() == "/Root/TestTable");
            UNIT_ASSERT(tables.at(1).GetMapSafe().at("name").GetStringSafe() == "/Root/TestTable/Index/indexImplTable");

            auto qId = session.PrepareDataQuery(query).ExtractValueSync().GetQuery();

            auto params = qId.GetParamsBuilder()
                .AddParam("$rows")
                .BeginList()
                .AddListItem()
                    .BeginStruct()
                        .AddMember("Key").OptionalString("Primary1")
                        .AddMember("Index2").OptionalString("Secondary1")
                        .AddMember("Value").OptionalString("Value1")
                    .EndStruct()
                .AddListItem()
                    .BeginStruct()
                        .AddMember("Key").OptionalString("Primary2")
                        .AddMember("Index2").OptionalString("Secondary2")
                        .AddMember("Value").OptionalString("Value2")
                    .EndStruct()
                .AddListItem()
                    .BeginStruct()
                        .AddMember("Key").OptionalString("Primary1")
                        .AddMember("Index2").OptionalString("Secondary11")
                        .AddMember("Value").OptionalString("Value1")
                    .EndStruct()
                .AddListItem()
                    .BeginStruct()
                        .AddMember("Key").OptionalString("Primary2")
                        .AddMember("Index2").OptionalString("Secondary22")
                        .AddMember("Value").OptionalString("Value2")
                    .EndStruct()
                .EndList()
                .Build()
            .Build();

            auto result = qId.Execute(
                TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx(),
                    std::move(params)).ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());

            CompareYson(R"([[["Secondary11"];["Primary1"]];[["Secondary22"];["Primary2"]]])",
                ReadTablePartToYson(session, "/Root/TestTable/Index/indexImplTable"));
        }
    }

    Y_UNIT_TEST_TWIN(MultipleModifications, UseNewEngine) {
        auto setting = NKikimrKqp::TKqpSetting();
        auto serverSettings = TKikimrSettings()
            .SetEnableMvcc(true)
            .SetEnableMvccSnapshotReads(true)
            .SetKqpSettings({setting});
        TKikimrRunner kikimr(serverSettings);
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        auto tableBuilder = db.GetTableBuilder();
        tableBuilder
            .AddNullableColumn("Key", EPrimitiveType::String)
            .AddNullableColumn("Index2", EPrimitiveType::String)
            .AddNullableColumn("Value", EPrimitiveType::String);
        tableBuilder.SetPrimaryKeyColumns(TVector<TString>{"Key"});
        tableBuilder.AddSecondaryIndex("Index", TVector<TString>{"Index2"});
        auto result = session.CreateTable("/Root/TestTable", tableBuilder.Build()).ExtractValueSync();
        UNIT_ASSERT_VALUES_EQUAL(result.IsTransportError(), false);
        UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), EStatus::SUCCESS);

        {
            const TString query1(Q_(R"(
                UPSERT INTO `/Root/TestTable` (Key, Index2, Value) VALUES
                    ("Primary1", "Secondary1", "Value1"),
                    ("Primary2", "Secondary2", "Value2");
            )"));

            auto result = session.ExecuteDataQuery(query1, TTxControl::BeginTx()).ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());

            auto tx = result.GetTransaction();


            const TString query2(Q_(R"(
                DELETE FROM `/Root/TestTable` ON (Key) VALUES
                    ("Primary1"),
                    ("Primary2");
            )"));

            result = session.ExecuteDataQuery(query2, TTxControl::Tx(*tx).CommitTx()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::GENERIC_ERROR, result.GetIssues().ToString());

            const auto& yson = ReadTablePartToYson(session, "/Root/TestTable/Index/indexImplTable");
            UNIT_ASSERT_VALUES_EQUAL(yson, "");
        }
    }

    template <bool UseNewEngine>
    void CreateTableWithIndexSQL(EIndexTypeSql type, bool enableAsyncIndexes = false) {
        auto kqpSetting = NKikimrKqp::TKqpSetting();
        kqpSetting.SetName("_KqpYqlSyntaxVersion");
        kqpSetting.SetValue("1");

        auto settings = TKikimrSettings()
                .SetEnableMvcc(true)
                .SetEnableMvccSnapshotReads(true)
                .SetEnableAsyncIndexes(enableAsyncIndexes)
                .SetKqpSettings({kqpSetting});
        TKikimrRunner kikimr(settings);
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        const auto typeStr = IndexTypeSqlString(type);
        const auto expectedStatus = (type == EIndexTypeSql::GlobalAsync)
            ? (enableAsyncIndexes ? EStatus::SUCCESS : EStatus::GENERIC_ERROR)
            : EStatus::SUCCESS;

        const TString createTableSql = Sprintf("CREATE TABLE `/Root/TestTable` ("
                "    Key Int32, IndexA Int32, IndexB Int32, IndexC String, Value String,"
                "    PRIMARY KEY (Key),"
                "    INDEX SecondaryIndex1 %s ON (IndexA, IndexB),"
                "    INDEX SecondaryIndex2 %s ON (IndexC)"
                ")", typeStr.data(), typeStr.data());
        {
            auto result = session.ExecuteSchemeQuery(createTableSql).GetValueSync();
            UNIT_ASSERT_VALUES_EQUAL(result.IsTransportError(), false);
            UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), expectedStatus);
        }

        // Multiple create table requests with same scheme should be OK
        {

            auto result = session.ExecuteSchemeQuery(createTableSql).GetValueSync();
            UNIT_ASSERT_VALUES_EQUAL(result.IsTransportError(), false);
            UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), expectedStatus);
        }

        if (type == EIndexTypeSql::GlobalAsync && !enableAsyncIndexes) {
            return;
        }

        // Check we can use index (metadata is correct)
        const TString query1(Q_(R"(
            UPSERT INTO `/Root/TestTable` (Key, IndexA, IndexB, IndexC, Value) VALUES
            (1, 11, 111, "a", "Value1"),
            (2, 22, 222, "b", "Value2"),
            (3, 33, 333, "c", "Value3");
        )"));

        auto result = session.ExecuteDataQuery(
                query1,
                TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
            .ExtractValueSync();

        UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());

        auto waitForAsyncIndexContent = [&session](const TString& indexImplTable, const TString& expected) {
            while (true) {
                const auto& yson = ReadTablePartToYson(session, indexImplTable);
                if (yson == expected) {
                    break;
                }

                Sleep(TDuration::Seconds(1));
            }
        };

        {
            const TString indexImplTable = "/Root/TestTable/SecondaryIndex1/indexImplTable";
            const TString expected = R"([[[11];[111];[1]];[[22];[222];[2]];[[33];[333];[3]]])";
            if (!enableAsyncIndexes) {
                UNIT_ASSERT_VALUES_EQUAL(ReadTablePartToYson(session, indexImplTable), expected);
            } else {
                waitForAsyncIndexContent(indexImplTable, expected);
            }
        }
        {
            const TString indexImplTable = "/Root/TestTable/SecondaryIndex2/indexImplTable";
            const TString expected = R"([[["a"];[1]];[["b"];[2]];[["c"];[3]]])";
            if (!enableAsyncIndexes) {
                UNIT_ASSERT_VALUES_EQUAL(ReadTablePartToYson(session, indexImplTable), expected);
            } else {
                waitForAsyncIndexContent(indexImplTable, expected);
            }
        }
    }

    Y_UNIT_TEST_TWIN(CreateTableWithImplicitSyncIndexSQL, UseNewEngine) {
        CreateTableWithIndexSQL<UseNewEngine>(EIndexTypeSql::Global);
    }

    Y_UNIT_TEST_TWIN(CreateTableWithExplicitSyncIndexSQL, UseNewEngine) {
        CreateTableWithIndexSQL<UseNewEngine>(EIndexTypeSql::GlobalSync);
    }

    Y_UNIT_TEST_TWIN(CreateTableWithAsyncIndexSQLShouldFail, UseNewEngine) {
        CreateTableWithIndexSQL<UseNewEngine>(EIndexTypeSql::GlobalAsync);
    }

    Y_UNIT_TEST_TWIN(CreateTableWithAsyncIndexSQLShouldSucceed, UseNewEngine) {
        CreateTableWithIndexSQL<UseNewEngine>(EIndexTypeSql::GlobalAsync, true);
    }

    template <bool UseNewEngine>
    void SelectFromAsyncIndexedTable() {
        auto kqpSetting = NKikimrKqp::TKqpSetting();
        kqpSetting.SetName("_KqpYqlSyntaxVersion");
        kqpSetting.SetValue("1");

        auto settings = TKikimrSettings()
                .SetEnableMvcc(true)
                .SetEnableMvccSnapshotReads(true)
                .SetEnableAsyncIndexes(true)
                .SetKqpSettings({kqpSetting});
        TKikimrRunner kikimr(settings);
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        {
            auto result = session.ExecuteSchemeQuery(R"(
                CREATE TABLE `/Root/TestTable` (
                    Key Int32, Index Int32, Value String,
                    PRIMARY KEY (Key),
                    INDEX SecondaryIndex GLOBAL ASYNC ON (Index)
                )
            )").GetValueSync();
            UNIT_ASSERT_VALUES_EQUAL(result.IsTransportError(), false);
            UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), EStatus::SUCCESS);
        }

        const auto query = Q_("SELECT * FROM `/Root/TestTable` VIEW SecondaryIndex WHERE Index == 1;");
        auto queryId = session.PrepareDataQuery(query).ExtractValueSync().GetQuery();

        const auto variants = TVector<std::pair<TTxSettings, EStatus>>{
            {TTxSettings::SerializableRW(), UseNewEngine ? EStatus::PRECONDITION_FAILED : EStatus::GENERIC_ERROR},
            {TTxSettings::OnlineRO(), UseNewEngine ? EStatus::PRECONDITION_FAILED : EStatus::GENERIC_ERROR},
            {TTxSettings::StaleRO(), EStatus::SUCCESS},
        };

        for (const auto& [settings, status] : variants) {
            {
                auto result = session.ExecuteDataQuery(query, TTxControl::BeginTx(settings).CommitTx()).ExtractValueSync();
                UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), status, TStringBuilder() << "Unexpected status #1"
                    << ": expected# " << status
                    << ": got# " << result.GetStatus()
                    << ", settings# " << settings
                    << ", issues# " << result.GetIssues().ToString());
            }

            {
                auto result = queryId.Execute(TTxControl::BeginTx(settings).CommitTx()).ExtractValueSync();
                UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), status, TStringBuilder() << "Unexpected status #2"
                    << ": expected# " << status
                    << ": got# " << result.GetStatus()
                    << ", settings# " << settings
                    << ", issues# " << result.GetIssues().ToString());
            }
        }
    }

    Y_UNIT_TEST_TWIN(SelectFromAsyncIndexedTable, UseNewEngine) {
        SelectFromAsyncIndexedTable<UseNewEngine>();
    }

    Y_UNIT_TEST_TWIN(InnerJoinWithNonIndexWherePredicate, UseNewEngine) {
        auto setting = NKikimrKqp::TKqpSetting();
        setting.SetName("_KqpYqlSyntaxVersion");
        setting.SetValue("1");
        auto serverSettings = TKikimrSettings()
            .SetEnableMvcc(true)
            .SetEnableMvccSnapshotReads(true)
            .SetKqpSettings({setting});
        TKikimrRunner kikimr(serverSettings);
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();
        CreateSampleTablesWithIndex(session);

        NYdb::NTable::TExecDataQuerySettings execSettings;
        execSettings.CollectQueryStats(ECollectQueryStatsMode::Basic);

        {
            const TString query1(Q_(R"(
                DECLARE $mongo_key AS Utf8?;
                DECLARE $targets AS List<Struct<fk1:Int32>>;
                DECLARE $limit AS Uint64;

                SELECT t.Value AS value
                FROM AS_TABLE($targets) AS k
                INNER JOIN `/Root/SecondaryComplexKeys` VIEW Index AS t
                ON t.Fk1 = k.fk1
                WHERE Fk2 > $mongo_key
                LIMIT $limit;
            )"));

            auto result = session.ExplainDataQuery(
                query1)
            .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());

            UNIT_ASSERT_C(!result.GetAst().Contains("EquiJoin"), result.GetAst());

            auto params = TParamsBuilder()
                .AddParam("$targets")
                    .BeginList()
                        .AddListItem()
                            .BeginStruct()
                                .AddMember("fk1").Int32(1)
                            .EndStruct()
                    .EndList().Build()
                .AddParam("$mongo_key")
                    .OptionalUtf8("")
                    .Build()
                .AddParam("$limit")
                    .Uint64(2)
                    .Build()
                .Build();


            auto result2 = session.ExecuteDataQuery(
                query1,
                TTxControl::BeginTx().CommitTx(),
                params,
                execSettings).ExtractValueSync();

            UNIT_ASSERT_C(result2.IsSuccess(), result2.GetIssues().ToString());
            UNIT_ASSERT(result2.GetIssues().Empty());

            UNIT_ASSERT_VALUES_EQUAL(NYdb::FormatResultSetYson(result2.GetResultSet(0)), "[[[\"Payload1\"]]]");

            auto& stats = NYdb::TProtoAccessor::GetProto(*result2.GetStats());

            int readPhase = 0;
            if (UseNewEngine) {
                UNIT_ASSERT_VALUES_EQUAL(stats.query_phases().size(), 3);
                readPhase = 1;
            }

            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(readPhase).table_access().size(), 1);
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(readPhase).table_access(0).name(), "/Root/SecondaryComplexKeys/Index/indexImplTable");
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(readPhase).table_access(0).reads().rows(), 1);

            readPhase++;

            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(readPhase).table_access().size(), 1);
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(readPhase).table_access(0).name(), "/Root/SecondaryComplexKeys");
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(readPhase).table_access(0).reads().rows(), 1);
        }
    }

    //KIKIMR-8144
    Y_UNIT_TEST_TWIN(InnerJoinSecondaryIndexLookupAndRightTablePredicateNonIndexColumn, UseNewEngine) {
        auto setting = NKikimrKqp::TKqpSetting();
        setting.SetName("_KqpYqlSyntaxVersion");
        setting.SetValue("1");
        auto serverSettings = TKikimrSettings()
            .SetEnableMvcc(true)
            .SetEnableMvccSnapshotReads(true)
            .SetKqpSettings({setting});
        TKikimrRunner kikimr(serverSettings);
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        const TString createTableSql = R"(CREATE TABLE `/Root/user` (
            id Uint64,
            uid Uint64,
            yandexuid Utf8,
            PRIMARY KEY (id),
            INDEX uid_index GLOBAL ON (uid),
            INDEX yandexuid_index GLOBAL ON (yandexuid)
        );)";
        {
            auto result = session.ExecuteSchemeQuery(createTableSql).GetValueSync();
            UNIT_ASSERT_VALUES_EQUAL(result.IsTransportError(), false);
            UNIT_ASSERT(result.GetIssues().Empty());
            UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), EStatus::SUCCESS);
        }

        {
            const TString query1(Q_(R"(
                UPSERT INTO `/Root/user` (id, yandexuid, uid) VALUES
                (2, "def", 222),
                (1, "abc", NULL),
                (NULL, "ghi", 333);
            )"));

            auto result = session.ExecuteDataQuery(
                query1,
                TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
            .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
        }

        {
            const auto& yson = ReadTablePartToYson(session, "/Root/user");
            const TString expected = R"([[#;[333u];["ghi"]];[[1u];#;["abc"]];[[2u];[222u];["def"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }

        auto execQuery = [&session](const TString& query) {
            auto result = session.ExecuteDataQuery(
                query,
                TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
            .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
            return NYdb::FormatResultSetYson(result.GetResultSet(0));
        };

        const TString query_template(Q_(R"(
            $yandexuids = (AsList(AsStruct(CAST("abc" as Utf8) as yandexuid), AsStruct(CAST("def" as Utf8) as yandexuid)));
            SELECT t.id as id, t.yandexuid as yandexuid, t.uid as uid
                FROM AS_TABLE($yandexuids) AS k
                %s JOIN `/Root/user` VIEW yandexuid_index AS t
                ON t.yandexuid = k.yandexuid
                %s
            ;)"));

        {
            const TString query = Sprintf(query_template.data(), "INNER", "WHERE uid IS NULL");
            const TString expected = R"([[[1u];["abc"];#]])";
            UNIT_ASSERT_VALUES_EQUAL(execQuery(query), expected);
        }

        {
            const TString query = Sprintf(query_template.data(), "INNER", "WHERE uid IS NOT NULL");
            const TString expected = R"([[[2u];["def"];[222u]]])";
            UNIT_ASSERT_VALUES_EQUAL(execQuery(query), expected);
        }

        {
            const TString query = Sprintf(query_template.data(), "LEFT", "WHERE uid IS NULL");
            const TString expected = R"([[[1u];["abc"];#]])";
            UNIT_ASSERT_VALUES_EQUAL(execQuery(query), expected);
        }

        {
            const TString query = Sprintf(query_template.data(), "LEFT", "WHERE uid IS NOT NULL");
            const TString expected = R"([[[2u];["def"];[222u]]])";
            UNIT_ASSERT_VALUES_EQUAL(execQuery(query), expected);
        }

        {
            const TString query = Sprintf(query_template.data(), "RIGHT", "WHERE uid IS NULL");
            const TString expected = R"([[[1u];["abc"];#]])";
            UNIT_ASSERT_VALUES_EQUAL(execQuery(query), expected);
        }

        {
            const TString query = Sprintf(query_template.data(), "RIGHT", "WHERE uid IS NOT NULL ORDER BY id DESC");
            const TString expected = R"([[[2u];["def"];[222u]];[#;["ghi"];[333u]]])";
            UNIT_ASSERT_VALUES_EQUAL(execQuery(query), expected);
        }

        {
            const TString query = Sprintf(query_template.data(), "RIGHT", "WHERE uid IS NOT NULL ORDER BY id");
            const TString expected = R"([[#;["ghi"];[333u]];[[2u];["def"];[222u]]])";
            UNIT_ASSERT_VALUES_EQUAL(execQuery(query), expected);
        }

        {
            const TString query = Sprintf(query_template.data(), "RIGHT", "WHERE uid IS NOT NULL ORDER BY yandexuid");
            const TString expected = R"([[[2u];["def"];[222u]];[#;["ghi"];[333u]]])";
            UNIT_ASSERT_VALUES_EQUAL(execQuery(query), expected);
        }

        {
            const TString query = Sprintf(query_template.data(), "RIGHT", "WHERE uid IS NOT NULL ORDER BY yandexuid DESC");
            const TString expected = R"([[#;["ghi"];[333u]];[[2u];["def"];[222u]]])";
            UNIT_ASSERT_VALUES_EQUAL(execQuery(query), expected);
        }

        {
            const TString query = Sprintf(query_template.data(), "RIGHT", "WHERE uid IS NOT NULL ORDER BY uid");
            const TString expected = R"([[[2u];["def"];[222u]];[#;["ghi"];[333u]]])";
            UNIT_ASSERT_VALUES_EQUAL(execQuery(query), expected);
        }

        {
            const TString query = Sprintf(query_template.data(), "RIGHT", "WHERE uid IS NOT NULL ORDER BY uid DESC");
            const TString expected = R"([[#;["ghi"];[333u]];[[2u];["def"];[222u]]])";
            UNIT_ASSERT_VALUES_EQUAL(execQuery(query), expected);
        }

        {
            const TString query = Sprintf(query_template.data(), "RIGHT", "WHERE uid IS NOT NULL");
            const TString expected = R"([[[2u];["def"];[222u]];[#;["ghi"];[333u]]])";
            UNIT_ASSERT_VALUES_EQUAL(execQuery(query), expected);
        }

        {
            const TString query = Sprintf(query_template.data(), "RIGHT ONLY", "WHERE uid IS NULL");
            const TString expected = R"([])";
            UNIT_ASSERT_VALUES_EQUAL(execQuery(query), expected);
        }

        {
            const TString query = Sprintf(query_template.data(), "RIGHT ONLY", "WHERE uid IS NOT NULL");
            const TString expected = R"([[#;["ghi"];[333u]]])";
            UNIT_ASSERT_VALUES_EQUAL(execQuery(query), expected);
        }

        {
            const TString query = Sprintf(query_template.data(), "RIGHT SEMI", "WHERE uid IS NULL");
            const TString expected = R"([[[1u];["abc"];#]])";
            UNIT_ASSERT_VALUES_EQUAL(execQuery(query), expected);
        }

        {
            const TString query = Sprintf(query_template.data(), "RIGHT SEMI", "WHERE uid IS NOT NULL");
            const TString expected = R"([[[2u];["def"];[222u]]])";
            UNIT_ASSERT_VALUES_EQUAL(execQuery(query), expected);
        }

        // Without using index by pk directly
        {
            const TString query1(Q_(R"(
                $ids = (AsList(AsStruct(CAST(1 as Uint64) as id), AsStruct(CAST(2 as Uint64) as id)));

                SELECT t.id as id, t.yandexuid as yandexuid, t.uid as uid
                    FROM AS_TABLE($ids) AS k
                    INNER JOIN `/Root/user` AS t
                    ON t.id = k.id
                    WHERE uid IS NULL
                ;)"));
            const TString expected = R"([[[1u];["abc"];#]])";
            UNIT_ASSERT_VALUES_EQUAL(execQuery(query1), expected);
        }
    }

    Y_UNIT_TEST_TWIN(DeleteByIndex, UseNewEngine) {
        auto setting = NKikimrKqp::TKqpSetting();
        auto serverSettings = TKikimrSettings()
            .SetEnableMvcc(true)
            .SetEnableMvccSnapshotReads(true)
            .SetKqpSettings({setting});
        TKikimrRunner kikimr(serverSettings);

        TScriptingClient client(kikimr.GetDriver());
        auto scriptResult = client.ExecuteYqlScript(R"(
            --!syntax_v1
            CREATE TABLE TestTable (
                Key Int32,
                Subkey Utf8,
                Value Utf8,
                PRIMARY KEY (Key, Subkey),
                INDEX SecondaryIndex GLOBAL ON (Subkey)
            );

            COMMIT;

            UPSERT INTO TestTable (Key, Subkey, Value) VALUES
                (1, "One", "Value1"),
                (1, "Two", "Value2"),
                (2, "One", "Value3"),
                (3, "One", "Value4");
        )").GetValueSync();
        UNIT_ASSERT_VALUES_EQUAL_C(scriptResult.GetStatus(), EStatus::SUCCESS, scriptResult.GetIssues().ToString());

        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        auto query = Q1_(R"(
            DECLARE $subkey AS Utf8;
            DECLARE $keys AS List<Int32>;

            $to_delete =
                SELECT Key FROM TestTable VIEW SecondaryIndex
                WHERE Subkey = $subkey AND Key NOT IN $keys;

            DELETE FROM TestTable
            WHERE Key IN $to_delete;
        )");

        auto explainResult = session.ExplainDataQuery(query).ExtractValueSync();
        UNIT_ASSERT_VALUES_EQUAL_C(explainResult.GetStatus(), EStatus::SUCCESS, explainResult.GetIssues().ToString());

        NJson::TJsonValue plan;
        NJson::ReadJsonTree(explainResult.GetPlan(), &plan, true);
        // NJson::WriteJson(&Cerr, &plan["tables"], true);

        auto tablePlan = FindPlanNodeByKv(plan["tables"], "name", "/Root/TestTable");
        UNIT_ASSERT(tablePlan.IsDefined());
        // TODO: KIKIMR-14074 (Unnecessary left semi join with own index table)
        UNIT_ASSERT_VALUES_EQUAL(tablePlan["reads"].GetArraySafe().size(), 1);
        for (const auto& read : tablePlan["reads"].GetArraySafe()) {
            UNIT_ASSERT_VALUES_UNEQUAL(read["type"].GetString(), "");
            UNIT_ASSERT_VALUES_UNEQUAL(read["type"].GetString(), "FullScan");
        }

        auto indexPlan = FindPlanNodeByKv(plan["tables"], "name", "/Root/TestTable/SecondaryIndex/indexImplTable");
        UNIT_ASSERT(indexPlan.IsDefined());
        for (const auto& read : indexPlan["reads"].GetArraySafe()) {
            UNIT_ASSERT_VALUES_UNEQUAL(read["type"].GetString(), "");
            UNIT_ASSERT_VALUES_UNEQUAL(read["type"].GetString(), "FullScan");
        }

        auto params = db.GetParamsBuilder()
            .AddParam("$subkey")
                .Utf8("One")
                .Build()
            .AddParam("$keys")
                .BeginList()
                    .AddListItem().Int32(1)
                .EndList()
                .Build()
            .Build();

        auto result = session.ExecuteDataQuery(query, TTxControl::BeginTx().CommitTx(), params).ExtractValueSync();
        UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());

        result = session.ExecuteDataQuery(Q1_(R"(
            SELECT * FROM TestTable ORDER BY Key, Subkey;
        )"), TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx()).ExtractValueSync();

        CompareYson(R"([
            [[1];["One"];["Value1"]];
            [[1];["Two"];["Value2"]]
        ])", FormatResultSetYson(result.GetResultSet(0)));
    }

    Y_UNIT_TEST_TWIN(UpdateDeletePlan, UseNewEngine) {
        auto setting = NKikimrKqp::TKqpSetting();
        auto serverSettings = TKikimrSettings()
            .SetEnableMvcc(true)
            .SetEnableMvccSnapshotReads(true)
            .SetKqpSettings({setting});
        TKikimrRunner kikimr(serverSettings);

        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        auto schemeResult = session.ExecuteSchemeQuery(R"(
            --!syntax_v1
            CREATE TABLE TestTable (
                Key Int32,
                Subkey Utf8,
                Value Utf8,
                PRIMARY KEY (Key),
                INDEX SecondaryIndex GLOBAL ON (Subkey)
            );
        )").GetValueSync();
        UNIT_ASSERT_VALUES_EQUAL_C(schemeResult.GetStatus(), EStatus::SUCCESS, schemeResult.GetIssues().ToString());

        auto checkPlan = [](const TString& planJson, ui32 tableReads, ui32 tableWrites, TMaybe<ui32> indexWrites) {
            NJson::TJsonValue plan;
            NJson::ReadJsonTree(planJson, &plan, true);
            const auto& tables = plan["tables"];
            // NJson::WriteJson(&Cerr, &tables, true);

            auto tablePlan = FindPlanNodeByKv(tables, "name", "/Root/TestTable");
            auto indexPlan = FindPlanNodeByKv(tables, "name", "/Root/TestTable/SecondaryIndex/indexImplTable");

            UNIT_ASSERT_VALUES_EQUAL(tablePlan["reads"].GetArraySafe().size(), tableReads);
            UNIT_ASSERT_VALUES_EQUAL(tablePlan["writes"].GetArraySafe().size(), tableWrites);
            if (indexWrites) {
                UNIT_ASSERT_VALUES_EQUAL(indexPlan["writes"].GetArraySafe().size(), *indexWrites);
            }
        };

        auto result = session.ExplainDataQuery(Q1_(R"(
            UPDATE TestTable SET Subkey = "Updated" WHERE Value = "Value2";
        )")).ExtractValueSync();
        UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
        checkPlan(result.GetPlan(), 1, 1, 2);

        result = session.ExplainDataQuery(Q1_(R"(
            UPDATE TestTable SET Value = "Updated" WHERE Value = "Value2";
        )")).ExtractValueSync();
        UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
        checkPlan(result.GetPlan(), 1, 1, {});

        result = session.ExplainDataQuery(Q1_(R"(
            DELETE FROM TestTable WHERE Value = "Value2";
        )")).ExtractValueSync();
        UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
        checkPlan(result.GetPlan(), 1, 1, 1);
    }

    Y_UNIT_TEST_TWIN(UpsertNoIndexColumns, UseNewEngine) {
        auto setting = NKikimrKqp::TKqpSetting();
        auto serverSettings = TKikimrSettings()
            .SetEnableMvcc(true)
            .SetEnableMvccSnapshotReads(true)
            .SetKqpSettings({setting});
        TKikimrRunner kikimr(serverSettings);

        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();
        CreateSampleTablesWithIndex(session);

        auto params = db.GetParamsBuilder()
            .AddParam("$rows")
                .BeginList()
                .AddListItem()
                    .BeginStruct()
                        .AddMember("Key").Int32(2)
                        .AddMember("Value").String("Upsert2")
                    .EndStruct()
                .AddListItem()
                    .BeginStruct()
                        .AddMember("Key").Int32(3)
                        .AddMember("Value").String("Upsert3")
                    .EndStruct()
                .EndList()
                .Build()
            .Build();

        auto result = session.ExecuteDataQuery(Q1_(R"(
            DECLARE $rows AS List<Struct<'Key': Int32, 'Value': String>>;

            UPSERT INTO SecondaryKeys
            SELECT * FROM AS_TABLE($rows);
        )"), TTxControl::BeginTx().CommitTx(), params).ExtractValueSync();
        UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());

        result = session.ExecuteDataQuery(Q1_(R"(
            SELECT Key, Fk, Value FROM SecondaryKeys WHERE Key IN [2, 3] ORDER BY Key;
            SELECT Key FROM SecondaryKeys VIEW Index WHERE Fk IS NULL ORDER BY Key;
            SELECT Key FROM SecondaryKeys VIEW Index WHERE Fk = 2 ORDER BY Key;
        )"), TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx()).ExtractValueSync();

        CompareYson(R"([
            [[2];[2];["Upsert2"]];
            [[3];#;["Upsert3"]]
        ])", FormatResultSetYson(result.GetResultSet(0)));

        CompareYson(R"([[#];[[3]];[[7]]])", FormatResultSetYson(result.GetResultSet(1)));
        CompareYson(R"([[[2]]])", FormatResultSetYson(result.GetResultSet(2)));
    }

    Y_UNIT_TEST_TWIN(UpdateIndexSubsetPk, UseNewEngine) {
        auto setting = NKikimrKqp::TKqpSetting();
        auto serverSettings = TKikimrSettings()
            .SetEnableMvcc(true)
            .SetEnableMvccSnapshotReads(true)
            .SetKqpSettings({setting});
        TKikimrRunner kikimr(serverSettings);

        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        TScriptingClient client(kikimr.GetDriver());
        auto scriptResult = client.ExecuteYqlScript(R"(
            --!syntax_v1
            CREATE TABLE TestTable (
                Key1 Int32,
                Key2 Uint64,
                Key3 String,
                Value String,
                PRIMARY KEY (Key1, Key2, Key3),
                INDEX SecondaryIndex GLOBAL ON (Key1, Key3)
            );

            COMMIT;

            UPSERT INTO TestTable (Key1, Key2, Key3, Value) VALUES
                (1, 10, "One", "Value1"),
                (1, 20, "Two", "Value2"),
                (2, 30, "One", "Value3"),
                (2, 40, "Two", "Value4");
        )").GetValueSync();
        UNIT_ASSERT_VALUES_EQUAL_C(scriptResult.GetStatus(), EStatus::SUCCESS, scriptResult.GetIssues().ToString());

        NYdb::NTable::TExecDataQuerySettings execSettings;
        execSettings.CollectQueryStats(ECollectQueryStatsMode::Basic);

        auto result = session.ExecuteDataQuery(Q1_(R"(
            UPDATE TestTable ON
            SELECT Key1, Key2, Key3, "Updated" AS Value
            FROM TestTable VIEW SecondaryIndex
            WHERE Key1 = 1 AND Key3 = "Two";
        )"), TTxControl::BeginTx().CommitTx(), execSettings).ExtractValueSync();
        UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());

        AssertTableStats(result, "/Root/TestTable", {
            .ExpectedReads = 1,
            .ExpectedUpdates = 1
        });

        AssertTableStats(result, "/Root/TestTable/SecondaryIndex/indexImplTable", {
            .ExpectedReads = 1,
            .ExpectedUpdates = 0
        });

        result = session.ExecuteDataQuery(Q1_(R"(
            SELECT * FROM TestTable VIEW SecondaryIndex WHERE Key1 = 1 ORDER BY Key1, Key2, Key3;
        )"), TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx()).ExtractValueSync();

        CompareYson(R"([
            [[1];[10u];["One"];["Value1"]];
            [[1];[20u];["Two"];["Updated"]]
        ])", FormatResultSetYson(result.GetResultSet(0)));

        result = session.ExecuteDataQuery(Q1_(R"(
            UPDATE TestTable SET Value = "Updated2"
            WHERE Key1 = 2 AND Key2 = 30;
        )"), TTxControl::BeginTx().CommitTx(), execSettings).ExtractValueSync();
        UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());

        AssertTableStats(result, "/Root/TestTable", {
            .ExpectedReads = 1,
            .ExpectedUpdates = 1
        });

        AssertTableStats(result, "/Root/TestTable/SecondaryIndex/indexImplTable", {
            .ExpectedReads = 0,
            .ExpectedUpdates = 0
        });

        result = session.ExecuteDataQuery(Q1_(R"(
            SELECT * FROM TestTable VIEW SecondaryIndex WHERE Key1 = 2 ORDER BY Key1, Key2, Key3;
        )"), TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx()).ExtractValueSync();

        CompareYson(R"([
            [[2];[30u];["One"];["Updated2"]];
            [[2];[40u];["Two"];["Value4"]]
        ])", FormatResultSetYson(result.GetResultSet(0)));
    }

    Y_UNIT_TEST_TWIN(IndexMultipleRead, UseNewEngine) {
        TKikimrRunner kikimr;

        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();
        CreateSampleTablesWithIndex(session);

        NYdb::NTable::TExecDataQuerySettings execSettings;
        execSettings.CollectQueryStats(ECollectQueryStatsMode::Basic);

        auto params = db.GetParamsBuilder()
            .AddParam("$fks")
                .BeginList()
                .AddListItem().Int32(5)
                .AddListItem().Int32(10)
                .EndList()
                .Build()
            .Build();

        auto result = session.ExecuteDataQuery(Q1_(R"(
            DECLARE $fks AS List<Int32>;

            SELECT * FROM SecondaryKeys VIEW Index WHERE Fk IN $fks;
            SELECT COUNT(*) FROM SecondaryKeys VIEW Index WHERE Fk IN $fks;
        )"), TTxControl::BeginTx().CommitTx(), params, execSettings).ExtractValueSync();
        UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());

        AssertTableStats(result, "/Root/SecondaryKeys", {
            .ExpectedReads = 1
        });

        AssertTableStats(result, "/Root/SecondaryKeys/Index/indexImplTable", {
            .ExpectedReads = UseNewEngine ? 1 : 2,
        });

        CompareYson(R"([[[5];[5];["Payload5"]]])", FormatResultSetYson(result.GetResultSet(0)));
        CompareYson(R"([[1u]])", FormatResultSetYson(result.GetResultSet(1)));
    }
}

}
}
