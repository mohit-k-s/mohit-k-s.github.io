+++
title = "Python's Native Proxy Pattern"
date = "2025-08-08"
description = "Pythonic way to make a proxy"
tags = [
    "proxy"
]
+++
While making [cwgrep](https://github.com/mohit-k-s/cwgrep), I wanted to log every AWS call the tool made without wrapping each method manually.

Turns out, Python lets you do that cleanly.

## \_\_getattr\_\_

This is a Python dunder method that’s only called when:
The attribute named name is not found in the usual places (**\_\_dict\_\_**, class, etc.).
So if self.describe_log_groups doesn’t exist on the AWSInterceptor instance, Python calls **\_\_getattr\_\_**('describe_log_groups').
This lets you act as a transparent proxy, forwarding unknown method accesses to the real client.



```python
import time 
import functools

SENSITIVE_KEYS = {"password", "secret", "token", "key"}

def sanitize(params):
    def _sanitize(obj):
        if isinstance(obj, dict):
            return {k: "***" if k.lower() in SENSITIVE_KEYS else _sanitize(v) for k, v in obj.items()}
        if isinstance(obj, list) and len(obj) > 10:
            return f"[{len(obj)} items]"
        return obj
    return _sanitize(params)

class AWSInterceptor:
    def __init__(self, client, logger):
        self._client = client
        self._logger = logger

    def __getattr__(self, name):
        method = getattr(self._client, name)
        if not callable(method):
            return method

        @functools.wraps(method)
        def wrapped(*args, **kwargs):
            start = time.time()
            try:
                result = method(*args, **kwargs)
                duration = time.time() - start
                self._logger.log_call(name, duration, success=True, params=sanitize(kwargs))
                return result
            except Exception as e:
                duration = time.time() - start
                self._logger.log_call(name, duration, success=False, error=str(e), params=sanitize(kwargs))
                raise

        return wrapped
```


Notice the weird decorator ?
```python
        @functools.wraps(method)
        def wrapped(*args, **kwargs):
```
This is classic decorator technique, but applied dynamically.
It preserves metadata like \_\_name\_\_, \_\_doc\_\_, and signature, so logs and debuggers still refer to the original method, not wrapped.


Now i only had to do 

```python
real_client = boto3.client("logs")
proxy = AWSInterceptor(real_client, logger)
proxy.describe_log_groups()
```

That’s it.


