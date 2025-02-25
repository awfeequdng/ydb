```go
package main

import (
	"context"
	"os"
	
	"github.com/ydb-platform/ydb-go-sdk/v3"
	"github.com/ydb-platform/ydb-go-sdk/v3/table"
	"github.com/ydb-platform/ydb-go-sdk/v3/table/types"
)

func main() {
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	db, err := ydb.Open(ctx,
		os.Getenv("YDB_CONNECTION_STRING"),
		ydb.WithAccessTokenCredentials(os.Getenv("YDB_TOKEN")),
	)
	if err != nil {
		panic(err)
	}
	defer func() { 
		_ = db.Close(ctx) 
	}()
	type logMessage struct {
		App       string
		Host      string
		Timestamp time.Time
		HTTPCode  uint32
		Message   string
	}
	// prepare native go data
	const batchSize = 10000
	logs := make([]logMessage, 0, batchSize)
	for i := 0; i < batchSize; i++ {
		message := logMessage{
			App:       fmt.Sprintf("App_%d", i/256),
			Host:      fmt.Sprintf("192.168.0.%d", i%256),
			Timestamp: time.Now().Add(time.Millisecond * time.Duration(i%1000)),
			HTTPCode:  200,
		}
		if i%2 == 0 {
			message.Message = "GET / HTTP/1.1"
		} else {
			message.Message = "GET /images/logo.png HTTP/1.1"
		}
		logs = append(logs, message)
	}
	// execute bulk upsert with native ydb data
	err = db.Table().Do( // Do retry operation on errors with best effort
		ctx, // context manages exiting from Do
		func(ctx context.Context, s table.Session) (err error) { // retry operation
			rows := make([]types.Value, 0, len(logs))
			for _, msg := range logs {
				rows = append(rows, types.StructValue(
					types.StructFieldValue("App", types.UTF8Value(msg.App)),
					types.StructFieldValue("Host", types.UTF8Value(msg.Host)),
					types.StructFieldValue("Timestamp", types.TimestampValueFromTime(msg.Timestamp)),
					types.StructFieldValue("HTTPCode", types.Uint32Value(msg.HTTPCode)),
					types.StructFieldValue("Message", types.UTF8Value(msg.Message)),
				))
			}
			return s.BulkUpsert(ctx, "/local/bulk_upsert_example", types.ListValue(rows...))
		},
	)
	if err != nil {
		fmt.Printf("unexpected error: %v", err)
	}
}
```
