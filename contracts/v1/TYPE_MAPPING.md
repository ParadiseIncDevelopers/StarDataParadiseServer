# V1 value encoding

Every value is a JSON object with a lower-case `t` type tag. The outer JSON envelope uses JSON null for optional metadata and the explicit null tag for data values.

| Type | JSON representation |
| --- | --- |
| NULL | `{"t":"null"}` |
| Boolean | `{"t":"bool","v":true}` |
| Integer | `{"t":"i64","v":"-9223372036854775808"}`. Range −2^63 through 2^63−1. Canonical base-10 digits, optional minus sign, no leading zeros and no negative zero. |
| Exact decimal | `{"t":"decimal","unscaled":"-123400","scale":4}`. Value is the unscaled integer × 10^−scale. Unscaled uses the same digit syntax with 1 to 38 digits. Scale is a JSON integer in [0,38]. Preserve scale and trailing zeros. |
| Text | `{"t":"text","v":""}`. Valid UTF-8 with scalar values including zero via JSON escaping. Preserve the scalar sequence without Unicode normalization. Empty text differs from NULL. |
| Binary | `{"t":"binary","hex":"00ff"}`. Two lower-case hex digits per byte, with empty hex for zero bytes. |
| Timestamp | `{"t":"timestamp","seconds":"0","nanos":0,"offset_minutes":180,"zone":"Europe/Istanbul"}`. Signed Unix seconds use i64 syntax, nanos are 0 through 999999999 and offset is −840 through 840 minutes. Optional `zone` records an IANA name. The instant is defined by epoch seconds plus nanos. The offset and zone preserve the display context. |
| Record | `{"t":"record","fields":[{"name":"k","value":{"t":"null"}}]}`. Fields preserve order, names are unique and values may contain records. |

A result `batch` contains zero-based `result_set` and `batch_index`, ordered column metadata, rows of ordered typed values, `end_of_set` and `end_of_results`. Column metadata is required in the first batch of each set. A result may have zero rows. An empty final batch can conclude a set. Clients validate order, row widths and field counts before consuming values.
