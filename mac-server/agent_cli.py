"""
Standalone CLI for the Pony P1 agent.

Lets you test the filesystem / vision tool-calling loop without the ESP32
or the HTTP server. Reads the same .env as server.py.

Usage:
  # One-off:
  python3 agent_cli.py "list the files in my folder"
  python3 agent_cli.py "describe IMG_6912.PNG"

  # Interactive REPL:
  python3 agent_cli.py
"""

from __future__ import annotations

import asyncio
import sys

from dotenv import load_dotenv

load_dotenv()

import agent  # noqa: E402 (must follow load_dotenv)


async def run_once(prompt: str) -> None:
    print(f"\n=== PROMPT ===\n{prompt}\n")
    try:
        answer, trace, elapsed = await agent.run_agent(prompt)
    except Exception as e:
        print(f"[agent error] {e}")
        return
    print(f"\n=== ANSWER ({elapsed:.1f}s, {len(trace)} tool calls) ===")
    print(answer or "(empty)")


async def repl() -> None:
    print(f"Pony P1 agent CLI")
    print(f"  model   : {agent.OLLAMA_MODEL}")
    print(f"  vision  : {agent.OLLAMA_VISION_MODEL}")
    print(f"  sandbox : {agent.AGENT_ROOT}")
    agent._ensure_sandbox()
    print("Type a prompt, or 'quit' to exit.\n")
    while True:
        try:
            prompt = input("you> ").strip()
        except (EOFError, KeyboardInterrupt):
            print()
            return
        if not prompt:
            continue
        if prompt.lower() in {"quit", "exit", "q"}:
            return
        await run_once(prompt)


def main() -> None:
    if len(sys.argv) > 1:
        prompt = " ".join(sys.argv[1:])
        asyncio.run(run_once(prompt))
    else:
        asyncio.run(repl())


if __name__ == "__main__":
    main()
