# Создание топика

С помощью подкоманды `topic create` вы можете создать новый топик.

Общий вид команды:

```bash
{{ ydb-cli }} [global options...] topic create [options...] <topic-path>
```

* `global options` — [глобальные параметры](commands/global-options.md).
* `options` — [параметры подкоманды](#options).
* `topic-path` — путь топика.

Посмотрите описание команды создания топика:

```bash
{{ ydb-cli }} topic create --help
```

## Параметры подкоманды {#options}

Имя | Описание
---|---
`--partitions-count VAL`| Количество [партиций](../../concepts/topic.md#partitioning) топика.<br>Значение по умолчанию — `1`.
`--retention-period-hours VAL` | Время хранения данных в топике, задается в часах.<br>Значение по умолчанию — `18`.
`--supported-codecs STRING` | Поддерживаемые методы сжатия данных.<br>Значение по умолчанию — `raw,zstd,gzip,lzop`.<br>Возможные значения:<ul><li>`RAW` — без сжатия;</li><li>`ZSTD` — сжатие [zstd](https://ru.wikipedia.org/wiki/Zstandard);</li><li>`GZIP` — сжатие [gzip](https://ru.wikipedia.org/wiki/Gzip);</li><li>`LZOP` — сжатие [lzop](https://ru.wikipedia.org/wiki/Lzop).</li></ul>

## Примеры {examples}

{% include [ydb-cli-profile](../../_includes/ydb-cli-profile.md) %}

Создание топика с 2 партициями, методами сжатия `RAW` и `GZIP`, временем хранения сообщений 2 часа и путем `my-topic`:

```bash
{{ ydb-cli }} -p db1 topic create \
  --partitions-count 2 \
  --supported-codecs raw,gzip \
  --retention-period-hours 2 \
  my-topic
```

Посмотрите параметры созданного топика:

```bash
{{ ydb-cli }} -p db1 scheme describe my-topic
```

Результат:

```text
RetentionPeriod: 2 hours
PartitionsCount: 2
SupportedCodecs: RAW, GZIP
```
