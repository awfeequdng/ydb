# Secondary indexes

[Indexes]{% if lang == "ru" %}(https://ru.wikipedia.org/wiki/Индекс_(базы_данных)){% endif %}{% if lang == "en" %}(https://en.wikipedia.org/wiki/Database_index){% endif %} are auxiliary database structures that are used to locate data based on specific criteria without searching the entire database. They are also used to retrieve sorted samples without sorting, which would require processing the full dataset.

Data in YDB tables is always sorted by the primary key. This means that regardless of the total number of table entries, retrieving an entry from the database with specific values in primary key fields will always take the minimum amount of time. Indexing by the primary key makes it possible to retrieve any consecutive range of entries in ascending or descending order of the primary key. The execution time for this operation depends only on the number of retrieved entries rather than the total number of table values.

To use a similar feature with any field or combination of fields, additional indexes, called **secondary indexes**, can be created for them.

In transactional systems, indexes are used to limit or avoid performance degradation and increase of query cost as your data grows.

This article describes basic operations with secondary indexes and gives references to a detailed description of each operation. For more information about various types of secondary indexes and their specific features, see [Secondary indexes](../../concepts/secondary_indexes.md) in the Concepts section.

## Creating secondary indexes {#create}

A secondary index is a data schema object that can be set when creating a table with the [`CREATE TABLE` YQL command](../../yql/reference/syntax/create_table.md) or added to it later with the [`ALTER TABLE` YQL command](../../yql/reference/syntax/alter_table.md).

The [`table index add` command](../../reference/ydb-cli/commands/secondary_index.md#add) is supported in the YDB CLI.

Since an index contains its own data derived from table data, when creating an index on an existing table with data, an operation is performed to initially build an index. This may take a long time. This operation is executed in the background and you can continue to work with the table while it's in progress. However, you can't use a new index until it's created.

An index can only be used in the order of the fields included in it. If an index contains two fields, such as `a` and `b`, you can effectively use it for queries such as:
* `WHERE a = $var1 AND b = $var2`.
* `WHERE a = $var1`.
* `WHERE a > $var1` and other comparison operators.
* `WHERE a = $var1 AND b > $var2` and any other comparison operators in which the first field must be checked for equality.

This index can't be used in the following queries:
* `WHERE b = $var1`.
* `WHERE a > $var1 AND b > $var2`, which is equivalent to `WHERE a > $var1` in terms of applying the index.
* `WHERE b > $var1`.

Considering the above, there's no use in pre-indexing all possible combinations of table columns to speed up the execution of any query. An index is always a compromise between the lookup and write speed and the storage space occupied by the data. Indexes are created for specific search queries and criteria made by an app in the database.

## Using secondary indexes when selecting data {#use}

For a table to be accessed by a secondary index, its name must be explicitly specified in the `VIEW` section after the table name as described in the article about the YQL [`SELECT` statement](../../yql/reference/syntax/select#secondary_index). For example, a query to retrieve orders from the `orders` table by the specified customer ID (`id_customer`) looks like this:

```sql
DECLARE $customer_id AS Uint64;
SELECT *
FROM   orders VIEW idx_customer AS o
WHERE  o.id_customer = $customer_id
```

where `idx_customer` is the name of the secondary index on the `orders` table with the `id_customer` field specified first.

If no `VIEW` section is specified, making a query like this requires a full scan of the `orders` table .

In transactional applications, such information queries are executed with paginated data results. This eliminates an increase in the cost and time of query execution if the number of entries that meet the filtering conditions grows. The described approach to writing [paginated queries](../paging.md) using the primary key can also be applied to columns that are part of a secondary index.

## Checking the cost of queries {#cost}

Any query made in a transactional application should be checked in terms of the number of I/O operations it performed in the database and how much CPU was used to run it. You should also make sure these indicators don't continuously grow as the database volume grows. YDB returns statistics required for the analysis after running each query.

If you use the YDB CLI, select the `--stats` option to enable printing statistics after executing the `yql` command. All YDB SDKs also contain structures with statistics returned after running a query. If you make a query in the UI, you'll see a tab with statistics next to the results tab.

## Updating data using a secondary index {#update}

The [`UPDATE`](../../yql/reference/syntax/update.md), [`UPSERT`](../../yql/reference/syntax/upsert_into.md), and [`REPLACE`](../../yql/reference/syntax/replace_into.md) YQL statements don't allow indicating the use of a secondary index to perform a search for data, so an attempt to make an `UPDATE ... WHERE indexed_field = $value` will result in a full scan of the table. To avoid this, you can first run `SELECT` by index to get the primary key value and then `UPDATE` by the primary key. You can also use `UPDATE ON`.

To update data in the `table1` table, run the query:

```sql
$to_update = (
    SELECT pk_field, $f1 AS field1, $f2 AS field2, ...
    FROM   table1 VIEW idx_field3
    WHERE  field3 = $f3)

UPDATE table1 ON SELECT * FROM $to_update
```

## Deleting data using a secondary index {#delete}

To delete data by secondary index, use `SELECT` with a predicate by secondary index and then call `DELETE ON`.

To delete all data about series with zero views from the `series` table, run the query:

```sql
DELETE FROM series ON
SELECT series_id
FROM series VIEW views_index
WHERE views = 0;
```

## Performance of data writes to tables with secondary indexes {#write_performance}

You need additional data structures to enable secondary indexes. Support for these structures increases the cost of table data update operations.

During synchronous index updates, a transaction is only committed after all the necessary data is written both in a table and synchronous indexes. As a result, it takes longer to execute it and makes it necessary to use [distributed transactions](../../concepts/transactions#distributed-tx) even if adding or updating entries in a single partition.

Indexes that are updated asynchronously let you use single-shard transactions. However, they only guarantee eventual consistency and still create a load on the database.
