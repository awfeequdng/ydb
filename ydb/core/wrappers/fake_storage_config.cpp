#include "fake_storage.h"
#include "fake_storage_config.h"
#include <library/cpp/actors/core/log.h>

#ifndef KIKIMR_DISABLE_S3_OPS
namespace NKikimr::NWrappers::NExternalStorage {

TString TFakeExternalStorageConfig::DoGetStorageId() const {
    return "fake";
}

IExternalStorageOperator::TPtr TFakeExternalStorageConfig::DoConstructStorageOperator() const {
    return std::make_shared<TFakeExternalStorageOperator>();
}
}

#endif // KIKIMR_DISABLE_S3_OPS
