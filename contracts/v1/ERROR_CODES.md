# V1 error codes

An error object contains a stable numeric `code`, a five-character `sqlstate`, a static `message`, nullable `line` and `column` (1-based), bool `retryable`, bool `unknown_outcome` and nullable `correlation_id`. Messages omit credentials, sensitive paths and request contents. Correlation IDs are server-issued opaque strings.

| Code | Name | SQLSTATE |
| ---: | --- | --- |
| 1001 | MALFORMED_MESSAGE | 08P01 |
| 1002 | MESSAGE_TOO_LARGE | 54000 |
| 1003 | INCOMPATIBLE_PROTOCOL | 0A000 |
| 1004 | FEATURE_UNSUPPORTED | 0A000 |
| 1005 | AUTH_FAILED | 28000 |
| 1006 | PERMISSION_DENIED | 42501 |
| 1007 | MODULE_UNAVAILABLE | 55000 |
| 1008 | TIMEOUT | HYT00 |
| 1009 | CANCELLED | 57014 |
| 1010 | CONNECTION_LOST | 08006 |
| 1011 | TLS_FAILED | 08001 |
| 1012 | INVALID_ARGUMENT | 22023 |
| 1013 | INVALID_STATE | 55000 |
| 1014 | RESOURCE_LIMIT | 53000 |
| 1015 | INTERNAL_ERROR | XX000 |
| 1016 | OUT_OF_MEMORY | HY001 |

`retryable` states that the reported condition permits a retry when the caller can determine the operation is safe. V1 foundation responses set it false. A connection loss after a mutating request could set `unknown_outcome` true. The caller must obtain a definitive transaction status before attempting any retry. No mutation is available in the initial service.
