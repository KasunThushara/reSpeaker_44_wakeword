#!/usr/bin/env python3
"""Probe free OpenRouter models and report which answer fastest/at all."""
import json
import os
import time
import urllib.error
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
CFG = os.path.join(HERE, "..", "pc_brain", "config.json")

with open(CFG, "r", encoding="utf-8-sig") as fh:
    cfg = json.load(fh)
key = str(cfg.get("openrouter_key", "")).strip()
if not key:
    raise SystemExit("no key in config.json")

MODELS = [
    "google/gemma-4-26b-a4b-it:free",
    "google/gemma-4-31b-it:free",
    "nvidia/nemotron-3.5-lightning:free",
    "liquid/lfm-3-1.7b:free",
    "inclusionai/ling-3.2-flash:free",
    "baidu/ernie-5.5-flash-preview:free",
    "recursive/flint-v1.0:free",
    "particle-ai/particle-1:free",
    "openrouter/free",
]

for model in MODELS:
    body = json.dumps({
        "model": model,
        "messages": [
            {"role": "system", "content": "Answer in one short sentence."},
            {"role": "user", "content": "Say one surprising fact about robots."},
        ],
        "max_tokens": 60,
        "temperature": 0.7,
    }).encode()
    req = urllib.request.Request(
        "https://openrouter.ai/api/v1/chat/completions",
        data=body,
        headers={
            "Authorization": "Bearer " + key,
            "Content-Type": "application/json",
            "HTTP-Referer": "http://localhost:8080",
            "X-Title": "JARVIS probe",
        },
    )
    t0 = time.time()
    try:
        with urllib.request.urlopen(req, timeout=25) as resp:
            data = json.loads(resp.read().decode())
        text = (data.get("choices") or [{}])[0].get("message", {}).get("content", "")
        ms = int((time.time() - t0) * 1000)
        print("%-42s %4dms  OK   %s" % (model, ms, (text or "<empty>").strip()[:70]))
    except urllib.error.HTTPError as exc:
        ms = int((time.time() - t0) * 1000)
        print("%-42s %4dms  HTTP %s" % (model, ms, exc.code))
    except Exception as exc:
        ms = int((time.time() - t0) * 1000)
        print("%-42s %4dms  ERR  %s" % (model, ms, str(exc)[:60]))
