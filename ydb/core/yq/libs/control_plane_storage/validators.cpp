#include "validators.h"
#include "ydb_control_plane_storage_impl.h"

#include <ydb/public/api/protos/yq.pb.h>

#include <ydb/core/yq/libs/control_plane_storage/events/events.h>
#include <ydb/core/yq/libs/db_schema/db_schema.h>

namespace NYq {

TValidationQuery CreateUniqueNameValidator(const TString& tableName,
                                           YandexQuery::Acl::Visibility visibility,
                                           const TString& scope,
                                           const TString& name,
                                           const TString& user,
                                           const TString& error,
                                           const TString& tablePathPrefix) {

    TSqlQueryBuilder queryBuilder(tablePathPrefix);
    queryBuilder.AddString("scope", scope);
    queryBuilder.AddString("name", name);
    queryBuilder.AddInt64("visibility", visibility);
    queryBuilder.AddText(
        "SELECT COUNT(*) AS count\n"
        "FROM `" + tableName + "` WHERE `" SCOPE_COLUMN_NAME "` = $scope AND `" NAME_COLUMN_NAME "` = $name AND `" VISIBILITY_COLUMN_NAME "` = $visibility"
    );

    if (visibility != YandexQuery::Acl::SCOPE) {
        queryBuilder.AddString("user", user);
        queryBuilder.AddText(" AND `" USER_COLUMN_NAME "` = $user");
    }

    auto validator = [error](NYdb::NTable::TDataQueryResult result) {
        const auto& resultSets = result.GetResultSets();
        if (resultSets.size() != 1) {
            ythrow TControlPlaneStorageException(TIssuesIds::INTERNAL_ERROR) << "Result set size is not equal to 1 but equal " << resultSets.size() << ". Please contact internal support";
        }

        TResultSetParser parser(resultSets.front());
        if (!parser.TryNextRow()) {
            ythrow TControlPlaneStorageException(TIssuesIds::INTERNAL_ERROR) << "Not valid number of lines, one is expected. Please contact internal support";
        }

        ui64 countNames = parser.ColumnParser("count").GetUint64();
        if (countNames != 0) {
            ythrow TControlPlaneStorageException(TIssuesIds::BAD_REQUEST) << error;
        }

        return false;
    };
    const auto query = queryBuilder.Build();
    return {query.Sql, query.Params, validator};
}

TValidationQuery CreateModifyUniqueNameValidator(const TString& tableName,
                                                 const TString& idColumnName,
                                                 YandexQuery::Acl::Visibility visibility,
                                                 const TString& scope,
                                                 const TString& name,
                                                 const TString& user,
                                                 const TString& id,
                                                 const TString& error,
                                                 const TString& tablePathPrefix) {
    TSqlQueryBuilder queryBuilder(tablePathPrefix);
    queryBuilder.AddString("scope", scope);
    queryBuilder.AddString("name", name);
    queryBuilder.AddString("id", id);
    queryBuilder.AddInt64("visibility", visibility);
    queryBuilder.AddText(
        "SELECT `" VISIBILITY_COLUMN_NAME "`, `" NAME_COLUMN_NAME "`\n"
        "FROM `" + tableName + "` WHERE `" SCOPE_COLUMN_NAME "` = $scope AND `" + idColumnName + "` = $id;\n"
        "SELECT COUNT(*) as count\n"
        "FROM `" + tableName + "` WHERE `" SCOPE_COLUMN_NAME "` = $scope AND `" NAME_COLUMN_NAME "` = $name AND `" VISIBILITY_COLUMN_NAME "` = $visibility"
    );

    if (visibility != YandexQuery::Acl::SCOPE) {
        queryBuilder.AddString("user", user);
        queryBuilder.AddText(" AND `" USER_COLUMN_NAME "` = $user");
    }

    auto validator = [error, visibility, name](NYdb::NTable::TDataQueryResult result) {
        const auto& resultSets = result.GetResultSets();
        if (resultSets.size() != 2) {
            ythrow TControlPlaneStorageException(TIssuesIds::INTERNAL_ERROR) << "Result set size is not equal to 2 but equal " << resultSets.size() << ". Please contact internal support";
        }

        {
            TResultSetParser parser(resultSets.front());
            if (!parser.TryNextRow()) {
                ythrow TControlPlaneStorageException(TIssuesIds::INTERNAL_ERROR) << "Not valid number of lines, one is expected. Please contact internal support";
            }

            YandexQuery::Acl::Visibility oldVisibility = static_cast<YandexQuery::Acl::Visibility>(parser.ColumnParser(VISIBILITY_COLUMN_NAME).GetOptionalInt64().GetOrElse(YandexQuery::Acl::VISIBILITY_UNSPECIFIED));
            TString oldName = parser.ColumnParser(NAME_COLUMN_NAME).GetOptionalString().GetOrElse("");

            if (oldVisibility == visibility && oldName == name) {
                return false;
            }
        }

        TResultSetParser parser(resultSets.back());
        if (!parser.TryNextRow()) {
            ythrow TControlPlaneStorageException(TIssuesIds::INTERNAL_ERROR) << "Not valid number of lines, one is expected. Please contact internal support";
        }

        ui64 countNames = parser.ColumnParser("count").GetUint64();
        if (countNames != 0) {
            ythrow TControlPlaneStorageException(TIssuesIds::BAD_REQUEST) << error;
        }

        return false;
    };

    const auto query = queryBuilder.Build();
    return {query.Sql, query.Params, validator};
}

TValidationQuery CreateCountEntitiesValidator(const TString& scope,
                                              const TString& tableName,
                                              ui64 limit,
                                              const TString& error,
                                              const TString& tablePathPrefix) {
    TSqlQueryBuilder queryBuilder(tablePathPrefix);
    queryBuilder.AddString("scope", scope);
    queryBuilder.AddText(
        "SELECT COUNT(*) as count\n"
        "FROM `" + tableName + "` WHERE `" SCOPE_COLUMN_NAME "` = $scope;"
    );

    auto validator = [error, limit](NYdb::NTable::TDataQueryResult result) {
        const auto& resultSets = result.GetResultSets();
        if (resultSets.size() != 1) {
            ythrow TControlPlaneStorageException(TIssuesIds::INTERNAL_ERROR) << "Result set size is not equal to 1 but equal " << resultSets.size() << ". Please contact internal support";
        }

        TResultSetParser parser(resultSets.front());
        if (!parser.TryNextRow()) {
            ythrow TControlPlaneStorageException(TIssuesIds::INTERNAL_ERROR) << "Not valid number of lines, one is expected. Please contact internal support";
        }

        ui64 countEntities = parser.ColumnParser("count").GetUint64();
        if (countEntities >= limit) {
            ythrow TControlPlaneStorageException(TIssuesIds::BAD_REQUEST) << error;
        }

        return false;
    };
    const auto query = queryBuilder.Build();
    return {query.Sql, query.Params, validator};
}

TValidationQuery CreateRevisionValidator(const TString& tableName,
                                         const TString& columnName,
                                         const TString& scope,
                                         const TString& id,
                                         i64 previousRevision,
                                         const TString& error,
                                         const TString& tablePathPrefix) {
    TSqlQueryBuilder queryBuilder(tablePathPrefix);
    queryBuilder.AddString("scope", scope);
    queryBuilder.AddString("id", id);
    queryBuilder.AddText(
        "SELECT `" REVISION_COLUMN_NAME "`\n"
        "FROM `" + tableName + "` WHERE `" SCOPE_COLUMN_NAME "` = $scope AND `" + columnName + "` = $id;"
    );

    auto validator = [error, previousRevision](NYdb::NTable::TDataQueryResult result) {
        const auto& resultSets = result.GetResultSets();
        if (resultSets.size() != 1) {
            ythrow TControlPlaneStorageException(TIssuesIds::INTERNAL_ERROR) << "Result set size is not equal to 1 but equal " << resultSets.size() << ". Please contact internal support";
        }

        TResultSetParser parser(resultSets.front());
        if (!parser.TryNextRow()) {
            return false;
        }

        i64 revision = parser.ColumnParser(REVISION_COLUMN_NAME).GetOptionalInt64().GetOrElse(0);
        if (revision != previousRevision) {
            ythrow TControlPlaneStorageException(TIssuesIds::BAD_REQUEST) << error;
        }

        return false;
    };
    const auto query = queryBuilder.Build();
    return {query.Sql, query.Params, validator};
}

static TValidationQuery CreateAccessValidatorImpl(const TString& tableName,
                                       const TString& columnName,
                                       const TString& scope,
                                       const TString& id,
                                       TString user,
                                       const TString& error,
                                       TPermissions permissions,
                                       const TString& tablePathPrefix,
                                       TPermissions::TPermission privatePermission,
                                       TPermissions::TPermission publicPermission) {
    TSqlQueryBuilder queryBuilder(tablePathPrefix);
    queryBuilder.AddString("scope", scope);
    queryBuilder.AddString("id", id);
    queryBuilder.AddText(
        "SELECT `" VISIBILITY_COLUMN_NAME "`, `" USER_COLUMN_NAME "`\n"
        "FROM `" + tableName + "` WHERE `" SCOPE_COLUMN_NAME "` = $scope AND `" + columnName + "` = $id\n"
    );

    auto validator = [error, user, permissions, privatePermission, publicPermission](NYdb::NTable::TDataQueryResult result) {
        const auto& resultSets = result.GetResultSets();
        if (resultSets.size() != 1) {
            ythrow TControlPlaneStorageException(TIssuesIds::INTERNAL_ERROR) << "Result set size is not equal to 1 but equal " << resultSets.size() << ". Please contact internal support";
        }

        TResultSetParser parser(resultSets.front());
        if (!parser.TryNextRow()) {
            ythrow TControlPlaneStorageException(TIssuesIds::ACCESS_DENIED) << error;
        }

        TString queryUser = parser.ColumnParser(USER_COLUMN_NAME).GetOptionalString().GetOrElse("");
        YandexQuery::Acl::Visibility visibility = static_cast<YandexQuery::Acl::Visibility>(parser.ColumnParser(VISIBILITY_COLUMN_NAME).GetOptionalInt64().GetOrElse(YandexQuery::Acl::VISIBILITY_UNSPECIFIED));
        bool hasAccess = HasAccessImpl(permissions, visibility, queryUser, user, privatePermission, publicPermission);
        if (!hasAccess) {
            ythrow TControlPlaneStorageException(TIssuesIds::ACCESS_DENIED) << error;
        }

        return false;
    };
    const auto query = queryBuilder.Build();
    return {query.Sql, query.Params, validator};
}

TValidationQuery CreateViewAccessValidator(const TString& tableName,
                                           const TString& columnName,
                                           const TString& scope,
                                           const TString& id,
                                           TString user,
                                           const TString& error,
                                           TPermissions permissions,
                                           const TString& tablePathPrefix) {
    return CreateAccessValidatorImpl(
            tableName, columnName, scope,
            id, user, error, permissions, tablePathPrefix,
            TPermissions::VIEW_PRIVATE, TPermissions::VIEW_PUBLIC);
}

TValidationQuery CreateManageAccessValidator(const TString& tableName,
                                             const TString& columnName,
                                             const TString& scope,
                                             const TString& id,
                                             TString user,
                                             const TString& error,
                                             TPermissions permissions,
                                             const TString& tablePathPrefix) {
    return CreateAccessValidatorImpl(
            tableName, columnName, scope,
            id, user, error, permissions, tablePathPrefix,
            TPermissions::MANAGE_PRIVATE, TPermissions::MANAGE_PUBLIC);
}

TValidationQuery CreateRelatedBindingsValidator(const TString& scope,
                                                const TString& connectionId,
                                                const TString& error,
                                                const TString& tablePathPrefix) {
    TSqlQueryBuilder queryBuilder(tablePathPrefix);
    queryBuilder.AddString("scope", scope);
    queryBuilder.AddString("connection_id", connectionId);
    queryBuilder.AddText(
        "SELECT COUNT(*) as count\n"
        "FROM `" BINDINGS_TABLE_NAME "` WHERE `" SCOPE_COLUMN_NAME "` = $scope AND `" CONNECTION_ID_COLUMN_NAME "` = $connection_id;"
    );

    auto validator = [error](NYdb::NTable::TDataQueryResult result) {
        const auto& resultSets = result.GetResultSets();
        if (resultSets.size() != 1) {
            ythrow TControlPlaneStorageException(TIssuesIds::INTERNAL_ERROR) << "Result set size is not equal to 1 but equal " << resultSets.size() << ". Please contact internal support";
        }

        TResultSetParser parser(resultSets.front());
        if (!parser.TryNextRow()) {
            ythrow TControlPlaneStorageException(TIssuesIds::INTERNAL_ERROR) << "Not valid number of lines, one is expected. Please contact internal support";
        }

        ui64 countEntities = parser.ColumnParser("count").GetUint64();
        if (countEntities != 0) {
            ythrow TControlPlaneStorageException(TIssuesIds::BAD_REQUEST) << error;
        }

        return false;
    };
    const auto query = queryBuilder.Build();
    return {query.Sql, query.Params, validator};
}

TValidationQuery CreateConnectionExistsValidator(const TString& scope,
                                                 const TString& connectionId,
                                                 const TString& error,
                                                 TPermissions permissions,
                                                 const TString& user,
                                                 YandexQuery::Acl::Visibility bindingVisibility,
                                                 const TString& tablePathPrefix) {
    TSqlQueryBuilder queryBuilder(tablePathPrefix);
    queryBuilder.AddString("scope", scope);
    queryBuilder.AddString("connection_id", connectionId);
    queryBuilder.AddText(
        "SELECT `" VISIBILITY_COLUMN_NAME "`, `" USER_COLUMN_NAME "`\n"
        "FROM `" CONNECTIONS_TABLE_NAME "` WHERE `" SCOPE_COLUMN_NAME "` = $scope AND `" CONNECTION_ID_COLUMN_NAME "` = $connection_id;"
    );

    auto validator = [error, user, permissions, bindingVisibility](NYdb::NTable::TDataQueryResult result) {
        const auto& resultSets = result.GetResultSets();
        if (resultSets.size() != 1) {
            ythrow TControlPlaneStorageException(TIssuesIds::INTERNAL_ERROR) << "Result set size is not equal to 1 but equal " << resultSets.size() << ". Please contact internal support";
        }

        TResultSetParser parser(resultSets.front());
        if (!parser.TryNextRow()) {
            ythrow TControlPlaneStorageException(TIssuesIds::ACCESS_DENIED) << error;
        }

        YandexQuery::Acl::Visibility connectionVisibility = static_cast<YandexQuery::Acl::Visibility>(parser.ColumnParser(VISIBILITY_COLUMN_NAME).GetOptionalInt64().GetOrElse(YandexQuery::Acl::VISIBILITY_UNSPECIFIED));
        TString connectionUser = parser.ColumnParser(USER_COLUMN_NAME).GetOptionalString().GetOrElse("");

        if (bindingVisibility == YandexQuery::Acl::SCOPE && connectionVisibility == YandexQuery::Acl::PRIVATE) {
            ythrow TControlPlaneStorageException(TIssuesIds::BAD_REQUEST) << "Binding with SCOPE visibility cannot refer to connection with PRIVATE visibility";
        }

        if (!HasManageAccess(permissions, connectionVisibility, connectionUser, user)) {
            ythrow TControlPlaneStorageException(TIssuesIds::ACCESS_DENIED) << error;
        }

        return false;
    };
    const auto query = queryBuilder.Build();
    return {query.Sql, query.Params, validator};
}

TValidationQuery CreateTtlValidator(const TString& tableName,
                                    const TString& columnName,
                                    const TString& scope,
                                    const TString& id,
                                    const TString& error,
                                    const TString& tablePathPrefix) {
    TSqlQueryBuilder queryBuilder(tablePathPrefix);
    queryBuilder.AddString("scope", scope);
    queryBuilder.AddString("id", id);
    queryBuilder.AddTimestamp("now", TInstant::Now());
    queryBuilder.AddText(
        "SELECT `" EXPIRE_AT_COLUMN_NAME "`\n"
        "FROM `" + tableName + "` WHERE `" SCOPE_COLUMN_NAME "` = $scope AND `" + columnName + "` = $id AND (`" EXPIRE_AT_COLUMN_NAME "` is NULL OR `" EXPIRE_AT_COLUMN_NAME "` > $now);\n"
    );

    auto validator = [error](NYdb::NTable::TDataQueryResult result) {
        const auto& resultSets = result.GetResultSets();
        if (resultSets.size() != 1) {
            ythrow TControlPlaneStorageException(TIssuesIds::INTERNAL_ERROR) << "Result set size is not equal to 1 but equal " << resultSets.size() << ". Please contact internal support";
        }

        TResultSetParser parser(resultSets.front());
        if (!parser.TryNextRow()) {
            ythrow TControlPlaneStorageException(TIssuesIds::INTERNAL_ERROR) << error;
        }

        return false;
    };
    const auto query = queryBuilder.Build();
    return {query.Sql, query.Params, validator};
}

TValidationQuery CreateQueryComputeStatusValidator(const std::vector<YandexQuery::QueryMeta::ComputeStatus>& computeStatuses,
                                                   const TString& scope,
                                                   const TString& id,
                                                   const TString& error,
                                                   const TString& tablePathPrefix) {
    TSqlQueryBuilder queryBuilder(tablePathPrefix, "ComputeStatusValidator");
    queryBuilder.AddString("scope", scope);
    queryBuilder.AddString("query_id", id);

    queryBuilder.AddText(
        "SELECT `" QUERY_COLUMN_NAME "` FROM `" QUERIES_TABLE_NAME "`\n"
        "WHERE `" SCOPE_COLUMN_NAME "` = $scope AND `" QUERY_ID_COLUMN_NAME "` = $query_id;\n"
    );

    auto validator = [error, computeStatuses](NYdb::NTable::TDataQueryResult result) {
        const auto& resultSets = result.GetResultSets();
        if (resultSets.size() != 1) {
            ythrow TControlPlaneStorageException(TIssuesIds::INTERNAL_ERROR) << "Result set size is not equal to 1 but equal " << resultSets.size() << ". Please contact internal support";
        }

        TResultSetParser parser(resultSets.front());
        if (!parser.TryNextRow()) {
            ythrow TControlPlaneStorageException(TIssuesIds::BAD_REQUEST) << "Query does not exist or permission denied. Please check the id of the query or your access rights";
        }

        YandexQuery::Query query;
        if (!query.ParseFromString(*parser.ColumnParser(QUERY_COLUMN_NAME).GetOptionalString())) {
            ythrow TControlPlaneStorageException(TIssuesIds::INTERNAL_ERROR) << "Error parsing proto message for query. Please contact internal support";
        }

        const YandexQuery::QueryMeta::ComputeStatus status = query.meta().status();
        if (!IsIn(computeStatuses, status)) {
            ythrow TControlPlaneStorageException(TIssuesIds::BAD_REQUEST) << error;
        }

        return false;
    };
    const auto query = queryBuilder.Build();
    return {query.Sql, query.Params, validator};
}

} // namespace NYq
