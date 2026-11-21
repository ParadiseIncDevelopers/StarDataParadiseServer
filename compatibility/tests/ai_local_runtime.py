"""Opt-in real GGUF import/load/inference, using explicitly supplied local assets."""
import argparse
import json
from pathlib import Path
import urllib.request

from compatibility.ai.local_model import LocalModelStore, LlamaRuntime
from compatibility.ai.providers import Model, ProviderClient
from compatibility.ai.secrets import SecretStore


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runtime', type=Path, required=True)
    parser.add_argument('--model', type=Path, required=True)
    parser.add_argument('--sha256', required=True)
    parser.add_argument('--store', type=Path, required=True)
    args = parser.parse_args(argv)
    store = LocalModelStore(args.store)
    artifact = store.import_file(args.model, expected_sha256=args.sha256,
                                 authorized_root=args.model.resolve().parent)
    runtime = LlamaRuntime(args.runtime, store, threads=2, memory_limit_bytes=1024**3)
    endpoint = None
    with runtime.start(artifact['artifact_id'], context_tokens=512) as process:
        endpoint = process['endpoint']
        model = Model('local_gguf', artifact['artifact_id'], 'runtime-acceptance', endpoint=endpoint,
                      capabilities=frozenset({'temperature', 'stop'}))
        client = ProviderClient(SecretStore({}), local_tokens={endpoint: process['token']})
        # A small base TinyStories model establishes real inference rather than
        # instruction-following quality. A period is a bounded natural stop.
        response = client.generate(model, [{'role': 'user', 'content': 'Once upon a time there was a little cat'}],
                                   options={'max_output_tokens': 128, 'temperature': 0, 'stop': ['.'],
                                            'provider_timeout_ms': 30000})
        assert response.text.strip(), 'runtime returned no generated text'
        assert response.provider == 'local_gguf'
        assert response.model == artifact['artifact_id']
        assert not response.tool_calls
        tool_template_verified=process['tool_template_verified']
    # Closing the context must close the listening socket, not leave an orphan.
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    try:
        opener.open(endpoint.rsplit('/v1/', 1)[0] + '/health', timeout=1)
    except OSError:
        pass
    else:
        raise AssertionError('owned runtime remained reachable after context exit')
    print(json.dumps({'case': 'real-local-gguf', 'state': 'PASS', 'artifact_id': artifact['artifact_id'],
                      'bytes': artifact['bytes'], 'generated_characters': len(response.text),
                      'usage': response.usage, 'runtime_stopped': True,'tool_template_verified':tool_template_verified}, sort_keys=True))


if __name__ == '__main__': main()
