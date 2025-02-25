#include "yql_s3_provider_impl.h"

#include <ydb/library/yql/utils/log/log.h>
#include <ydb/library/yql/core/yql_opt_utils.h>
#include <ydb/library/yql/dq/expr_nodes/dq_expr_nodes.h>
#include <ydb/library/yql/dq/opt/dq_opt.h>
#include <ydb/library/yql/dq/opt/dq_opt_phy.h>
#include <ydb/library/yql/providers/common/transform/yql_optimize.h>
#include <ydb/library/yql/providers/s3/expr_nodes/yql_s3_expr_nodes.h>

namespace NYql {

namespace {

using namespace NNodes;
using namespace NDq;

TExprNode::TPtr GetPartitionBy(const TExprNode& settings) {
    for (auto i = 0U; i < settings.ChildrenSize(); ++i) {
        if (settings.Child(i)->Head().IsAtom("partitionedby")) {
            return settings.ChildPtr(i);
        }
    }

    return {};
}

TExprNode::TPtr GetCompression(const TExprNode& settings) {
    for (auto i = 0U; i < settings.ChildrenSize(); ++i) {
        if (settings.Child(i)->Head().IsAtom("compression")) {
            return settings.ChildPtr(i);
        }
    }

    return {};
}

TExprNode::TListType GetPartitionKeys(const TExprNode::TPtr& partBy) {
    if (partBy) {
        auto children = partBy->ChildrenList();
        children.erase(children.cbegin());
        return children;
    }

    return {};
}

TString GetExtension(const std::string_view& format, const std::string_view& compression) {
    static const std::unordered_map<std::string_view, std::string_view> formatsMap = {
        {"csv_with_names"sv, "csv"sv},
        {"tsv_with_names"sv, "tsv"sv},
        {"raw"sv, "bin"sv},
        {"json_list"sv, "json"sv},
        {"json_each_row"sv, "json"sv},
        {"parquet"sv, "parquet"sv}
    };

    static const std::unordered_map<std::string_view, std::string_view> compressionsMap = {
        {"gzip"sv, "gz"sv},
        {"zstd"sv, "zst"sv},
        {"lz4"sv, "lz4"sv},
        {"bzip2"sv, "bz2"sv},
        {"brotli"sv, "br"sv},
        {"xz"sv, "xz"sv}
    };

    TStringBuilder extension;
    if (const auto it = formatsMap.find(format); formatsMap.cend() != it) {
        extension << '.' << it->second;
    }

    if (const auto it = compressionsMap.find(compression); compressionsMap.cend() != it) {
        extension << '.' << it->second;
    }
    return extension;
}

class TS3PhysicalOptProposalTransformer : public TOptimizeTransformerBase {
public:
    explicit TS3PhysicalOptProposalTransformer(TS3State::TPtr state)
        : TOptimizeTransformerBase(state->Types, NLog::EComponent::ProviderS3, {})
        , State_(std::move(state))
    {
#define HNDL(name) "PhysicalOptimizer-"#name, Hndl(&TS3PhysicalOptProposalTransformer::name)
        AddHandler(0, &TS3WriteObject::Match, HNDL(S3WriteObject));
#undef HNDL
    }

    TMaybeNode<TExprBase> S3WriteObject(TExprBase node, TExprContext& ctx, const TGetParents& getParents) const {
        const auto& write = node.Cast<TS3WriteObject>();
        const auto& targetNode = write.Target();
        const auto& cluster = write.DataSink().Cluster().StringValue();
        const auto token = "cluster:default_" + cluster;
        auto partBy = GetPartitionBy(write.Target().Settings().Ref());
        auto keys = GetPartitionKeys(partBy);

        auto sinkSettingsBuilder = Build<TExprList>(ctx, targetNode.Pos());
        if (partBy)
            sinkSettingsBuilder.Add(std::move(partBy));

        auto compression = GetCompression(write.Target().Settings().Ref());
        const auto& extension = GetExtension(write.Target().Format().Value(), compression ? compression->Tail().Content() : ""sv);
        if (compression)
            sinkSettingsBuilder.Add(std::move(compression));

        if (!FindNode(write.Input().Ptr(), [] (const TExprNode::TPtr& node) { return node->IsCallable(TCoDataSource::CallableName()); })) {
            YQL_CLOG(INFO, ProviderS3) << "Rewrite pure S3WriteObject `" << cluster << "`.`" << targetNode.Path().StringValue() << "` as stage with sink.";
            return keys.empty() ?
                Build<TDqQuery>(ctx, write.Pos())
                    .World(write.World())
                    .SinkStages()
                        .Add<TDqStage>()
                            .Inputs().Build()
                            .Program<TCoLambda>()
                                .Args({})
                                .Body<TS3SinkOutput>()
                                    .Input<TCoToFlow>()
                                        .Input(write.Input())
                                        .Build()
                                    .Format(write.Target().Format())
                                    .KeyColumns().Build()
                                    .Build()
                                .Build()
                            .Outputs<TDqStageOutputsList>()
                                .Add<TDqSink>()
                                    .DataSink(write.DataSink())
                                    .Index().Value("0").Build()
                                    .Settings<TS3SinkSettings>()
                                        .Path(write.Target().Path())
                                        .Settings(sinkSettingsBuilder.Done())
                                        .Token<TCoSecureParam>()
                                            .Name().Build(token)
                                            .Build()
                                        .Extension().Value(extension).Build()
                                        .Build()
                                    .Build()
                                .Build()
                            .Settings().Build()
                            .Build()
                        .Build()
                    .Done():
                Build<TDqQuery>(ctx, write.Pos())
                    .World(write.World())
                    .SinkStages()
                        .Add<TDqStage>()
                            .Inputs()
                                .Add<TDqCnHashShuffle>()
                                    .Output<TDqOutput>()
                                        .Stage<TDqStage>()
                                            .Inputs().Build()
                                            .Program<TCoLambda>()
                                                .Args({})
                                                .Body<TCoToFlow>()
                                                    .Input(write.Input())
                                                    .Build()
                                                .Build()
                                            .Settings().Build()
                                            .Build()
                                        .Index().Value("0", TNodeFlags::Default).Build()
                                        .Build()
                                    .KeyColumns().Add(keys).Build()
                                    .Build()
                                .Build()
                            .Program<TCoLambda>()
                                .Args({"in"})
                                .Body<TS3SinkOutput>()
                                    .Input("in")
                                    .Format(write.Target().Format())
                                    .KeyColumns().Add(keys).Build()
                                    .Build()
                                .Build()
                            .Outputs<TDqStageOutputsList>()
                                .Add<TDqSink>()
                                    .DataSink(write.DataSink())
                                    .Index().Value("0", TNodeFlags::Default).Build()
                                    .Settings<TS3SinkSettings>()
                                        .Path(write.Target().Path())
                                        .Settings(sinkSettingsBuilder.Done())
                                        .Token<TCoSecureParam>()
                                            .Name().Build(token)
                                            .Build()
                                        .Extension().Value(extension).Build()
                                        .Build()
                                    .Build()
                                .Build()
                            .Settings().Build()
                            .Build()
                        .Build()
                    .Done();
        }

        if (!TDqCnUnionAll::Match(write.Input().Raw())) {
            return node;
        }

        const TParentsMap* parentsMap = getParents();
        const auto dqUnion = write.Input().Cast<TDqCnUnionAll>();
        if (!NDq::IsSingleConsumerConnection(dqUnion, *parentsMap)) {
            return node;
        }

        YQL_CLOG(INFO, ProviderS3) << "Rewrite S3WriteObject `" << cluster << "`.`" << targetNode.Path().StringValue() << "` as sink.";

        const auto inputStage = dqUnion.Output().Stage().Cast<TDqStage>();

        const auto sink = Build<TDqSink>(ctx, write.Pos())
            .DataSink(write.DataSink())
            .Index(dqUnion.Output().Index())
            .Settings<TS3SinkSettings>()
                .Path(write.Target().Path())
                .Settings(sinkSettingsBuilder.Done())
                .Token<TCoSecureParam>()
                    .Name().Build(token)
                    .Build()
                .Extension().Value(extension).Build()
                .Build()
            .Done();

        auto outputsBuilder = Build<TDqStageOutputsList>(ctx, targetNode.Pos());
        if (inputStage.Outputs() && keys.empty()) {
            outputsBuilder.InitFrom(inputStage.Outputs().Cast());
        }
        outputsBuilder.Add(sink);

        if (keys.empty()) {
            const auto outputBuilder = Build<TS3SinkOutput>(ctx, targetNode.Pos())
                .Input(inputStage.Program().Body().Ptr())
                .Format(write.Target().Format())
                .KeyColumns().Add(std::move(keys)).Build()
                .Done();

            return Build<TDqQuery>(ctx, write.Pos())
                .World(write.World())
                .SinkStages()
                    .Add<TDqStage>()
                        .InitFrom(inputStage)
                        .Program(ctx.DeepCopyLambda(inputStage.Program().Ref(), outputBuilder.Ptr()))
                        .Outputs(outputsBuilder.Done())
                        .Build()
                    .Build()
                .Done();
        } else {
            return Build<TDqQuery>(ctx, write.Pos())
                .World(write.World())
                .SinkStages()
                    .Add<TDqStage>()
                        .Inputs()
                            .Add<TDqCnHashShuffle>()
                                .Output<TDqOutput>()
                                    .Stage(inputStage)
                                    .Index(dqUnion.Output().Index())
                                    .Build()
                                .KeyColumns().Add(keys).Build()
                                .Build()
                            .Build()
                        .Program<TCoLambda>()
                            .Args({"in"})
                            .Body<TS3SinkOutput>()
                                .Input("in")
                                .Format(write.Target().Format())
                                .KeyColumns().Add(std::move(keys)).Build()
                                .Build()
                            .Build()
                        .Settings().Build()
                        .Outputs(outputsBuilder.Done())
                        .Build()
                    .Build()
                .Done();
        }
    }

private:
    const TS3State::TPtr State_;
};

} // namespace

THolder<IGraphTransformer> CreateS3PhysicalOptProposalTransformer(TS3State::TPtr state) {
    return MakeHolder<TS3PhysicalOptProposalTransformer>(std::move(state));
}

} // namespace NYql

