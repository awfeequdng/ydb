#pragma once

#ifndef KIKIMR_DISABLE_S3_OPS

#include "abstract.h"

#include <ydb/core/base/events.h>
#include <ydb/core/protos/flat_scheme_op.pb.h>
#include <ydb/public/api/protos/ydb_import.pb.h>

#include <contrib/libs/aws-sdk-cpp/aws-cpp-sdk-core/include/aws/core/auth/AWSCredentials.h>

#include <util/string/builder.h>
#include <util/string/printf.h>

namespace NKikimr::NWrappers::NExternalStorage {

struct TS3User {
    TS3User();
    TS3User(const TS3User& baseObject);
    TS3User(TS3User& baseObject);
    ~TS3User();
};

class TS3ExternalStorageConfig: public IExternalStorageConfig, TS3User {
private:
    Aws::Client::ClientConfiguration Config;
    const Aws::Auth::AWSCredentials Credentials;

    static Aws::Client::ClientConfiguration ConfigFromSettings(const NKikimrSchemeOp::TS3Settings& settings);
    static Aws::Auth::AWSCredentials CredentialsFromSettings(const NKikimrSchemeOp::TS3Settings& settings);
    static Aws::Client::ClientConfiguration ConfigFromSettings(const Ydb::Import::ImportFromS3Settings& settings);
    static Aws::Auth::AWSCredentials CredentialsFromSettings(const Ydb::Import::ImportFromS3Settings& settings);

protected:
    virtual TString DoGetStorageId() const override;
    virtual IExternalStorageOperator::TPtr DoConstructStorageOperator() const override;
public:
    const Aws::Client::ClientConfiguration& GetConfig() const {
        return Config;
    }

    Aws::Client::ClientConfiguration& ConfigRef() {
        return Config;
    }

    TS3ExternalStorageConfig(const NKikimrSchemeOp::TS3Settings& settings)
        : Config(ConfigFromSettings(settings))
        , Credentials(CredentialsFromSettings(settings)) {

    }
    TS3ExternalStorageConfig(const Ydb::Import::ImportFromS3Settings& settings)
        : Config(ConfigFromSettings(settings))
        , Credentials(CredentialsFromSettings(settings)) {

    }
    TS3ExternalStorageConfig(const Aws::Auth::AWSCredentials& credentials, const Aws::Client::ClientConfiguration& config)
        : Config(config)
        , Credentials(credentials) {

    }
};
} // NKikimr::NWrappers::NExternalStorage

#endif // KIKIMR_DISABLE_S3_OPS
