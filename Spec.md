# Discovery and Realtime Transport

Optionally-reliable UDP as main underlying transport

## Interface

3 primitive types of topics:
1. Variable
2. Stream
3. Method

Variables: multiple writers, multiple readers, stores the most recent value, optionally store value and history, query logical changes
Streams: single writer, multiple readers, optionally store value and history, query logical changes
Methods: multiple senders, multiple receivers (assuming no return value), req/resp, no stored values