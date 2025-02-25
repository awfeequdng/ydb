#include "kqp_scan_data.h"

#include <ydb/library/yql/public/udf/udf_ut_helpers.h>
#include <ydb/library/yql/minikql/mkql_alloc.h>
#include <library/cpp/testing/unittest/registar.h>

namespace NKikimr::NMiniKQL {

namespace {
namespace NTypeIds = NScheme::NTypeIds;
using TTypeId = NScheme::TTypeId;

struct TDataRow {
    TSmallVec<TKqpComputeContextBase::TColumn> Columns() {
        return {
            {0, NTypeIds::Bool},
            {1, NTypeIds::Int8},
            {2, NTypeIds::Int16},
            {3, NTypeIds::Int32},
            {4, NTypeIds::Int64},
            {5, NTypeIds::Uint8},
            {6, NTypeIds::Uint16},
            {7, NTypeIds::Uint32},
            {8, NTypeIds::Uint64},
            {9, NTypeIds::Float},
            {10, NTypeIds::Double},
            {11, NTypeIds::String},
            {12, NTypeIds::Utf8},
            {13, NTypeIds::Json},
            {14, NTypeIds::Yson},
            {15, NTypeIds::Date},
            {16, NTypeIds::Datetime},
            {17, NTypeIds::Timestamp},
            {18, NTypeIds::Interval},
            {19, NTypeIds::Decimal},
        };
    }

    bool Bool;
    i8 Int8;
    i16 Int16;
    i32 Int32;
    i64 Int64;
    ui8 UInt8;
    ui16 UInt16;
    ui32 UInt32;
    ui64 UInt64;
    float Float32;
    double Float64;
    TString String;
    TString Utf8;
    TString Json;
    TString Yson;
    i32 Date;
    i64 Datetime;
    i64 Timestamp;
    i64 Interval;
    NYql::NDecimal::TInt128 Decimal;

    static std::shared_ptr<arrow::Schema> MakeArrowSchema() {
        std::vector<std::shared_ptr<arrow::Field>> fields = {
            arrow::field("bool", arrow::boolean()),
            arrow::field("i8", arrow::int8()),
            arrow::field("i16", arrow::int16()),
            arrow::field("i32", arrow::int32()),
            arrow::field("i64", arrow::int64()),
            arrow::field("ui8", arrow::uint8()),
            arrow::field("ui16", arrow::uint16()),
            arrow::field("ui32", arrow::uint32()),
            arrow::field("ui64", arrow::uint64()),
            arrow::field("f32", arrow::float32()),
            arrow::field("f64", arrow::float64()),
            arrow::field("string", arrow::utf8()),
            arrow::field("utf8", arrow::utf8()),
            arrow::field("json", arrow::binary()),
            arrow::field("yson", arrow::binary()),
            arrow::field("date", arrow::date32()),
            arrow::field("datetime", arrow::timestamp(arrow::TimeUnit::TimeUnit::SECOND)),
            arrow::field("ts", arrow::timestamp(arrow::TimeUnit::TimeUnit::MICRO)),
            arrow::field("ival", arrow::duration(arrow::TimeUnit::TimeUnit::MICRO)),
            arrow::field("dec", arrow::decimal(NScheme::DECIMAL_PRECISION, NScheme::DECIMAL_SCALE)),
        };

        return std::make_shared<arrow::Schema>(fields);
    }
};

std::shared_ptr<arrow::RecordBatch> VectorToBatch(const std::vector<struct TDataRow>& rows, std::shared_ptr<arrow::Schema>&& resultSchema) {
    TString err;
    std::unique_ptr<arrow::RecordBatchBuilder> batchBuilder = nullptr;
    std::shared_ptr<arrow::RecordBatch> batch;
    auto result = arrow::RecordBatchBuilder::Make(resultSchema, arrow::default_memory_pool(), &batchBuilder);
    UNIT_ASSERT(result.ok());

    for (const TDataRow& row : rows) {
        int colIndex = 0;
        for (auto colName : resultSchema->field_names()) {
            if (colName == "bool") {
                auto result = batchBuilder->GetFieldAs<arrow::BooleanBuilder>(colIndex++)->Append(row.Bool);
                UNIT_ASSERT(result.ok());
            } else if (colName == "i8") {
                auto result = batchBuilder->GetFieldAs<arrow::Int8Builder>(colIndex++)->Append(row.Int8);
                UNIT_ASSERT(result.ok());
            } else if (colName == "i16") {
                auto result = batchBuilder->GetFieldAs<arrow::Int16Builder>(colIndex++)->Append(row.Int16);
                UNIT_ASSERT(result.ok());
            } else if (colName == "i32") {
                auto result = batchBuilder->GetFieldAs<arrow::Int32Builder>(colIndex++)->Append(row.Int32);
                UNIT_ASSERT(result.ok());
            } else if (colName == "i64") {
                auto result = batchBuilder->GetFieldAs<arrow::Int64Builder>(colIndex++)->Append(row.Int64);
                UNIT_ASSERT(result.ok());
            } else if (colName == "ui8") {
                auto result = batchBuilder->GetFieldAs<arrow::UInt8Builder>(colIndex++)->Append(row.UInt8);
                UNIT_ASSERT(result.ok());
            } else if (colName == "ui16") {
                auto result = batchBuilder->GetFieldAs<arrow::UInt16Builder>(colIndex++)->Append(row.UInt16);
                UNIT_ASSERT(result.ok());
            } else if (colName == "ui32") {
                auto result = batchBuilder->GetFieldAs<arrow::UInt32Builder>(colIndex++)->Append(row.UInt32);
                UNIT_ASSERT(result.ok());
            } else if (colName == "ui64") {
                auto result = batchBuilder->GetFieldAs<arrow::UInt64Builder>(colIndex++)->Append(row.UInt64);
                UNIT_ASSERT(result.ok());
            } else if (colName == "f32") {
                auto result = batchBuilder->GetFieldAs<arrow::FloatBuilder>(colIndex++)->Append(row.Float32);
                UNIT_ASSERT(result.ok());
            } else if (colName == "f64") {
                auto result = batchBuilder->GetFieldAs<arrow::DoubleBuilder>(colIndex++)->Append(row.Float64);
                UNIT_ASSERT(result.ok());
            } else if (colName == "string") {
                auto result = batchBuilder->GetFieldAs<arrow::StringBuilder>(colIndex++)->Append(row.String.data(), row.String.size());
                UNIT_ASSERT(result.ok());
            } else if (colName == "utf8") {
                auto result = batchBuilder->GetFieldAs<arrow::StringBuilder>(colIndex++)->Append(row.Utf8.data(), row.Utf8.size());
                UNIT_ASSERT(result.ok());
            } else if (colName == "json") {
                auto result = batchBuilder->GetFieldAs<arrow::BinaryBuilder>(colIndex++)->Append(row.Json.data(), row.Json.size());
                UNIT_ASSERT(result.ok());
            } else if (colName == "yson") {
                auto result = batchBuilder->GetFieldAs<arrow::BinaryBuilder>(colIndex++)->Append(row.Yson.data(), row.Yson.size());
                UNIT_ASSERT(result.ok());
            } else if (colName == "date") {
                auto result = batchBuilder->GetFieldAs<arrow::Date32Builder>(colIndex++)->Append(row.Date);
                UNIT_ASSERT(result.ok());
            } else if (colName == "datetime") {
                auto result = batchBuilder->GetFieldAs<arrow::TimestampBuilder>(colIndex++)->Append(row.Datetime);
                UNIT_ASSERT(result.ok());
            } else if (colName == "ts") {
                auto result = batchBuilder->GetFieldAs<arrow::TimestampBuilder>(colIndex++)->Append(row.Timestamp);
                UNIT_ASSERT(result.ok());
            } else if (colName == "ival") {
                auto result = batchBuilder->GetFieldAs<arrow::DurationBuilder>(colIndex++)->Append(row.Interval);
                UNIT_ASSERT(result.ok());
            } else if (colName == "dec") {
                auto result = batchBuilder->GetFieldAs<arrow::Decimal128Builder>(colIndex++)->Append(reinterpret_cast<const char*>(&row.Decimal));
                UNIT_ASSERT(result.ok());
            }
        }
    }

    auto resultFlush = batchBuilder->Flush(&batch);
    UNIT_ASSERT(resultFlush.ok());
    return batch;
}

TVector<TDataRow> TestRows() {
    TVector<TDataRow> rows = {
        {false, -1, -1, -1, -1, 1, 1, 1, 1, -1.0f, -1.0, "s1"                       , "u1"                      , "{j:1}", "{y:1}", 0, 0, 0, 0, 111},
        {false,  2,  2,  2,  2, 2, 2, 2, 2,  2.0f,  2.0, "s2"                       , "u2"                      , "{j:2}", "{y:2}", 0, 0, 0, 0, 222},
        {false, -3, -3, -3, -3, 3, 3, 3, 3, -3.0f, -3.0, "s3"                       , "u3"                      , "{j:3}", "{y:3}", 0, 0, 0, 0, 333},
        {false, -4, -4, -4, -4, 4, 4, 4, 4,  4.0f,  4.0, "s4"                       , "u4"                      , "{j:4}", "{y:4}", 0, 0, 0, 0, 444},
        {false, -5, -5, -5, -5, 5, 5, 5, 5,  5.0f,  5.0, "long5long5long5long5long5", "utflong5utflong5utflong5", "{j:5}", "{y:5}", 0, 0, 0, 0, 555},
    };
    return rows;
}

}

Y_UNIT_TEST_SUITE(TKqpScanData) {

    Y_UNIT_TEST(UnboxedValueSize) {
        NKikimr::NMiniKQL::TScopedAlloc alloc;
        namespace NTypeIds = NScheme::NTypeIds;
        struct TTestCase {
            NUdf::TUnboxedValue Value;
            NScheme::TTypeId Type;
            std::pair<ui64, ui64> ExpectedSizes;
        };
        TString pattern = "This string has 26 symbols";
        NUdf::TStringValue str(pattern.size());
        std::memcpy(str.Data(), pattern.data(), pattern.size());
        NUdf::TUnboxedValue containsLongString(NUdf::TUnboxedValuePod(std::move(str)));
        NYql::NDecimal::TInt128 decimalVal = 123456789012;
        TVector<TTestCase> cases = {
            {NUdf::TUnboxedValuePod(            ), NTypeIds::Bool        , {16, 8 } },
            {NUdf::TUnboxedValuePod(            ), NTypeIds::Int32       , {16, 8 } },
            {NUdf::TUnboxedValuePod(            ), NTypeIds::Uint32      , {16, 8 } },
            {NUdf::TUnboxedValuePod(            ), NTypeIds::Int64       , {16, 8 } },
            {NUdf::TUnboxedValuePod(            ), NTypeIds::Uint64      , {16, 8 } },
            {NUdf::TUnboxedValuePod(            ), NTypeIds::Double      , {16, 8 } },
            {NUdf::TUnboxedValuePod(            ), NTypeIds::Float       , {16, 8 } },
            {NUdf::TUnboxedValuePod(            ), NTypeIds::String      , {16, 8 } },
            {NUdf::TUnboxedValuePod(            ), NTypeIds::Utf8        , {16, 8 } },
            {NUdf::TUnboxedValuePod(            ), NTypeIds::Yson        , {16, 8 } },
            {NUdf::TUnboxedValuePod(            ), NTypeIds::Json        , {16, 8 } },
            {NUdf::TUnboxedValuePod(            ), NTypeIds::Decimal     , {16, 8 } },
            {NUdf::TUnboxedValuePod(            ), NTypeIds::Date        , {16, 8 } },
            {NUdf::TUnboxedValuePod(            ), NTypeIds::Datetime    , {16, 8 } },
            {NUdf::TUnboxedValuePod(            ), NTypeIds::Timestamp   , {16, 8 } },
            {NUdf::TUnboxedValuePod(            ), NTypeIds::Interval    , {16, 8 } },
            {NUdf::TUnboxedValuePod(true        ), NTypeIds::Bool        , {16, 8 } },
            {NUdf::TUnboxedValuePod((i8) 1      ), NTypeIds::Int8        , {16, 8 } },
            {NUdf::TUnboxedValuePod((i16) 2     ), NTypeIds::Int16       , {16, 8 } },
            {NUdf::TUnboxedValuePod((i32) 3     ), NTypeIds::Int32       , {16, 8 } },
            {NUdf::TUnboxedValuePod((i64) 4     ), NTypeIds::Int64       , {16, 8 } },
            {NUdf::TUnboxedValuePod((ui8) 5     ), NTypeIds::Uint8       , {16, 8 } },
            {NUdf::TUnboxedValuePod((ui16) 6    ), NTypeIds::Uint16      , {16, 8 } },
            {NUdf::TUnboxedValuePod((ui32) 7    ), NTypeIds::Uint32      , {16, 8 } },
            {NUdf::TUnboxedValuePod((ui64) 8    ), NTypeIds::Uint64      , {16, 8 } },
            {NUdf::TUnboxedValuePod((float) 1.2 ), NTypeIds::Float       , {16, 8 } },
            {NUdf::TUnboxedValuePod(123456789012), NTypeIds::Date        , {16, 8 } },
            {NUdf::TUnboxedValuePod((double) 3.4), NTypeIds::Double      , {16, 8 } },
            {NUdf::TUnboxedValuePod(123456789012), NTypeIds::Datetime    , {16, 8 } },
            {NUdf::TUnboxedValuePod(123456789012), NTypeIds::Timestamp   , {16, 8 } },
            {NUdf::TUnboxedValuePod(123456789012), NTypeIds::Interval    , {16, 8 } },
            {NUdf::TUnboxedValuePod(decimalVal  ), NTypeIds::Decimal     , {16, 16} },
            {NUdf::TUnboxedValuePod::Embedded("12charecters"), NTypeIds::String , {16, 12 } },
            {NUdf::TUnboxedValuePod::Embedded("foooo"), NTypeIds::String , {16, 8 } },
            {NUdf::TUnboxedValuePod::Embedded("FOOD!"), NTypeIds::Utf8   , {16, 8 } },
            {NUdf::TUnboxedValuePod::Embedded("{j:0}"), NTypeIds::Json   , {16, 8 } },
            {NUdf::TUnboxedValuePod::Embedded("{y:0}"), NTypeIds::Yson   , {16, 8 } },
            {containsLongString                       , NTypeIds::String, {16 + pattern.size(), pattern.size()}}
        };

        for (auto& testCase: cases) {
            auto sizes = GetUnboxedValueSizeForTests(testCase.Value, testCase.Type);
            UNIT_ASSERT_EQUAL_C(sizes, testCase.ExpectedSizes, "Wrong size for type " << NScheme::TypeName(testCase.Type));
        }
    }

    Y_UNIT_TEST(ArrowToUnboxedValueConverter) {
        TVector<TDataRow> rows = TestRows();
        std::shared_ptr<arrow::RecordBatch> batch = VectorToBatch(rows, rows.front().MakeArrowSchema());
        NKikimr::NMiniKQL::TScopedAlloc alloc;
        TMemoryUsageInfo memInfo("");
        THolderFactory factory(alloc.Ref(), memInfo);

        TKqpScanComputeContext::TScanData scanData({}, TTableRange({}), rows.front().Columns(), {}, {}, rows.front().Columns());

        scanData.AddRows(*batch, {}, factory);

        for (auto& row: rows) {
            auto result_row = scanData.TakeRow();
            UNIT_ASSERT_EQUAL(result_row.GetElement(0 ).Get<bool  >(), row.Bool   );
            UNIT_ASSERT_EQUAL(result_row.GetElement(1 ).Get<i8    >(), row.Int8   );
            UNIT_ASSERT_EQUAL(result_row.GetElement(2 ).Get<i16   >(), row.Int16  );
            UNIT_ASSERT_EQUAL(result_row.GetElement(3 ).Get<i32   >(), row.Int32  );
            UNIT_ASSERT_EQUAL(result_row.GetElement(4 ).Get<i64   >(), row.Int64  );
            UNIT_ASSERT_EQUAL(result_row.GetElement(5 ).Get<ui8   >(), row.UInt8  );
            UNIT_ASSERT_EQUAL(result_row.GetElement(6 ).Get<ui16  >(), row.UInt16 );
            UNIT_ASSERT_EQUAL(result_row.GetElement(7 ).Get<ui32  >(), row.UInt32 );
            UNIT_ASSERT_EQUAL(result_row.GetElement(8 ).Get<ui64  >(), row.UInt64 );
            UNIT_ASSERT_EQUAL(result_row.GetElement(9 ).Get<float >(), row.Float32);
            UNIT_ASSERT_EQUAL(result_row.GetElement(10).Get<double>(), row.Float64);
            auto tmpString = result_row.GetElement(11);
            UNIT_ASSERT_EQUAL(TString(tmpString.AsStringRef().Data()), row.String);
            auto tmpUtf8 = result_row.GetElement(12);
            UNIT_ASSERT_EQUAL(TString(tmpUtf8.AsStringRef().Data()), row.Utf8);
            auto tmpJson = result_row.GetElement(13);
            UNIT_ASSERT_EQUAL(TString(tmpJson.AsStringRef().Data()), row.Json);
            auto tmpYson = result_row.GetElement(14);
            UNIT_ASSERT_EQUAL(TString(tmpYson.AsStringRef().Data()), row.Yson);
            UNIT_ASSERT_EQUAL(result_row.GetElement(15).Get<i32 >(), row.Date     );
            UNIT_ASSERT_EQUAL(result_row.GetElement(16).Get<i64 >(), row.Datetime );
            UNIT_ASSERT_EQUAL(result_row.GetElement(17).Get<i64 >(), row.Timestamp);
            UNIT_ASSERT_EQUAL(result_row.GetElement(18).Get<i64 >(), row.Interval );
            UNIT_ASSERT_EQUAL(result_row.GetElement(19).GetInt128(), row.Decimal  );
        }

        UNIT_ASSERT(scanData.IsEmpty());

        scanData.Clear();
    }

    Y_UNIT_TEST(DifferentNumberOfInputAndResultColumns) {
        TVector<TDataRow> rows = TestRows();
        std::vector<std::shared_ptr<arrow::Field>> fields = { arrow::field("i8", arrow::int8()) };
        std::shared_ptr<arrow::RecordBatch> batch = VectorToBatch(rows, std::make_shared<arrow::Schema>(fields));
        NKikimr::NMiniKQL::TScopedAlloc alloc;
        TMemoryUsageInfo memInfo("");
        THolderFactory factory(alloc.Ref(), memInfo);

        TSmallVec<TKqpComputeContextBase::TColumn> resultCols;
        auto resCol = TKqpComputeContextBase::TColumn {
            .Type = NTypeIds::Int8
        };
        resultCols.push_back(resCol);
        TKqpScanComputeContext::TScanData scanData({}, TTableRange({}), rows.front().Columns(), {}, {}, resultCols);

        scanData.AddRows(*batch, {}, factory);

        for (auto& row: rows) {
            auto result_row = scanData.TakeRow();
            UNIT_ASSERT_EQUAL(result_row.GetElement(0).Get<i8>(), row.Int8);
        }

        UNIT_ASSERT(scanData.IsEmpty());

        scanData.Clear();
    }

    Y_UNIT_TEST(EmptyColumns) {
        NKikimr::NMiniKQL::TScopedAlloc alloc;
        TMemoryUsageInfo memInfo("");
        THolderFactory factory(alloc.Ref(), memInfo);

        TKqpScanComputeContext::TScanData scanData({}, TTableRange({}), {}, {}, {}, {});
        TVector<TOwnedCellVec> emptyBatch(1000);
        auto bytes = scanData.AddRows(emptyBatch, {}, factory);
        UNIT_ASSERT(bytes > 0);

        for (const auto& row: emptyBatch) {
            Y_UNUSED(row);
            UNIT_ASSERT(!scanData.IsEmpty());
            auto item = scanData.TakeRow();
            UNIT_ASSERT(item.GetListLength() == 0);
        }
        UNIT_ASSERT(scanData.IsEmpty());
    }

    Y_UNIT_TEST(EmptyColumnsAndNonEmptyArrowBatch) {
        NKikimr::NMiniKQL::TScopedAlloc alloc;
        TMemoryUsageInfo memInfo("");
        THolderFactory factory(alloc.Ref(), memInfo);
        TKqpScanComputeContext::TScanData scanData({}, TTableRange({}), {}, {}, {}, {});

        TVector<TDataRow> rows = TestRows();
        std::shared_ptr<arrow::RecordBatch> anotherEmptyBatch = VectorToBatch(rows, rows.front().MakeArrowSchema());

        auto bytes = scanData.AddRows(*anotherEmptyBatch, {}, factory);
        UNIT_ASSERT(bytes > 0);
        for (const auto& row: rows) {
            Y_UNUSED(row);
            UNIT_ASSERT(!scanData.IsEmpty());
            auto item = scanData.TakeRow();
            UNIT_ASSERT(item.GetListLength() == 0);
        }
        UNIT_ASSERT(scanData.IsEmpty());
    }
}

} // namespace NKikimr::NMiniKQL
