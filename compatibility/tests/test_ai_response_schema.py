import unittest
from compatibility.ai.contracts import AiError
from compatibility.ai.response_schema import check_schema, decode_response, validate_value


class ResponseSchemaTests(unittest.TestCase):
    def setUp(self):
        self.schema = {'type': 'object', 'required': ['order_id', 'status'], 'additionalProperties': False,
                       'properties': {'order_id': {'type': 'integer'}, 'status': {'type': 'string', 'minLength': 1},
                                      'tracking_code': {'type': ['string', 'null']}}}

    def test_typed_json_and_exact_bigint(self):
        value = decode_response('{"order_id":9007199254740993,"status":"tamam","tracking_code":null}', self.schema)
        self.assertIsInstance(value, dict)
        self.assertEqual(9007199254740993, value['order_id'])
        self.assertIsNone(value['tracking_code'])

    def test_schema_error_has_json_pointer_and_no_value(self):
        with self.assertRaises(AiError) as error:
            decode_response('{"order_id":true,"status":"tamam"}', self.schema)
        self.assertEqual(504003, error.exception.code)
        self.assertEqual({'path': '/order_id'}, error.exception.details)

    def test_missing_additional_and_empty_values(self):
        for text in ('{"order_id":1}', '{"order_id":1,"status":""}', '{"order_id":1,"status":"ok","other":1}'):
            with self.assertRaises(AiError): decode_response(text, self.schema)

    def test_remote_ref_and_unknown_keywords_rejected_at_registration(self):
        for schema in ({'$ref': 'https://example.invalid/schema'}, {'pattern': '(x+)+'}, {'format': 'email'}):
            with self.assertRaises(AiError) as error: check_schema(schema)
            self.assertEqual(502006, error.exception.code)

    def test_schema_definition_types_ranges_and_depth(self):
        for schema in ({'type': ['string', 'string']}, {'type': 'imaginary'}, {'minLength': -1}, {'required': [42]},
                       {'minimum': 5, 'maximum': 1}, {'properties': []}):
            with self.assertRaises(AiError): check_schema(schema)
        nested = True
        for _ in range(20): nested = {'type': 'array', 'items': nested}
        with self.assertRaises(AiError): check_schema(nested)

    def test_boolean_is_not_integer_enum(self):
        with self.assertRaises(AiError): validate_value(True, {'enum': [1]})
        self.assertEqual(1.0, validate_value(1.0, {'enum': [1]}))
        with self.assertRaises(AiError): validate_value({'value': True}, {'enum': [{'value': 1}]})

    def test_arrays_numbers_unicode_and_null_are_distinct(self):
        schema = {'type': 'array', 'minItems': 1, 'maxItems': 2, 'items': {'type': 'integer', 'minimum': 0, 'maximum': 5}}
        self.assertEqual([0, 5], validate_value([0, 5], schema))
        for value in ([], [6], [None], [1, 2, 3]):
            with self.assertRaises(AiError): validate_value(value, schema)
        self.assertIsNone(decode_response('null'))
        self.assertEqual('', decode_response('""'))
        self.assertEqual({}, decode_response('{}'))
        with self.assertRaises(AiError) as error: decode_response('"\ud800"')
        self.assertEqual(504007, error.exception.code)

    def test_invalid_and_truncated_response_are_separate(self):
        with self.assertRaises(AiError) as invalid: decode_response('{')
        with self.assertRaises(AiError) as truncated: decode_response('{"x":1}', maximum_bytes=2)
        self.assertEqual(504002, invalid.exception.code)
        self.assertEqual(504004, truncated.exception.code)


if __name__ == '__main__': unittest.main()
