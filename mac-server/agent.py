"""
Pony P1 agent: tool-calling loop backed by Ollama.

Exposes a small set of sandboxed filesystem tools to the LLM:
  - list_directory
  - read_text_file
  - write_text_file
  - read_pdf
  - describe_image   (round-trips to a separate Ollama vision model)

All tool paths are resolved relative to AGENT_ROOT; anything that would
escape the sandbox is rejected.
"""

from __future__ import annotations

import base64
import json
import os
import time
from pathlib import Path
from typing import Any

import httpx


# ── Config (read from env) ──────────────────────────────────
AGENT_ROOT = Path(
    os.path.expanduser(os.getenv("AGENT_ROOT", "~/Desktop/pony_agent"))
).resolve()

OLLAMA_URL   = os.getenv("OLLAMA_URL", "http://localhost:11434")
OLLAMA_MODEL = os.getenv("OLLAMA_MODEL", "qwen3.5:9b")
# If OLLAMA_VISION_MODEL is blank/unset, reuse OLLAMA_MODEL. This works great
# for multimodal models like qwen3.5 (text + image + tools in one model).
OLLAMA_VISION_MODEL = (os.getenv("OLLAMA_VISION_MODEL") or "").strip() or OLLAMA_MODEL

LLM_TIMEOUT_S    = 120.0
MAX_ITERATIONS   = 5       # tool-call rounds before giving up
MAX_FILE_CHARS   = 4000    # truncate file reads fed back to the model
MAX_IMAGE_BYTES  = 5 * 1024 * 1024  # 5 MB cap for describe_image

IMAGE_EXTS = {".png", ".jpg", ".jpeg", ".gif", ".bmp", ".webp"}


# ── Sandbox helpers ─────────────────────────────────────────
def _ensure_sandbox() -> None:
    AGENT_ROOT.mkdir(parents=True, exist_ok=True)
    welcome = AGENT_ROOT / "welcome.md"
    if not welcome.exists():
        welcome.write_text(
            "# Pony P1 agent sandbox\n\n"
            "This is the only folder the voice assistant can see.\n"
            "Drop .txt, .md, .pdf, .png, or .jpg files here and ask the device "
            "about them.\n\n"
            "Try asking: 'list the files', 'read welcome.md', "
            "'what's in screenshot.png'.\n"
        )


def safe_path(relpath: str) -> Path:
    """Resolve `relpath` under AGENT_ROOT, blocking any escape."""
    rel = (relpath or "").strip()
    # Strip leading slash so absolute-looking paths are treated as relative.
    while rel.startswith("/"):
        rel = rel[1:]
    p = (AGENT_ROOT / rel).resolve()
    try:
        p.relative_to(AGENT_ROOT)
    except ValueError:
        raise ValueError(f"path escapes sandbox: {relpath!r}")
    return p


# ── Tool implementations ────────────────────────────────────
def tool_list_directory(path: str = "") -> str:
    p = safe_path(path)
    if not p.exists():
        return f"Error: '{path}' does not exist."
    if not p.is_dir():
        return f"Error: '{path}' is not a directory."
    entries: list[str] = []
    for item in sorted(p.iterdir()):
        rel = item.relative_to(AGENT_ROOT)
        if item.is_dir():
            entries.append(f"dir            {rel}/")
        else:
            entries.append(f"file {item.stat().st_size:>10}  {rel}")
    return "\n".join(entries) if entries else "(empty)"


def tool_read_text_file(path: str) -> str:
    p = safe_path(path)
    if not p.exists():
        return f"Error: '{path}' does not exist."
    if not p.is_file():
        return f"Error: '{path}' is not a file."
    try:
        data = p.read_text(errors="replace")
    except UnicodeDecodeError:
        return f"Error: '{path}' is not a text file."
    except Exception as e:
        return f"Error reading: {e}"
    if len(data) > MAX_FILE_CHARS:
        return data[:MAX_FILE_CHARS] + (
            f"\n\n...(truncated, {len(data) - MAX_FILE_CHARS} more chars)"
        )
    return data


def tool_write_text_file(path: str, content: str) -> str:
    p = safe_path(path)
    if p.exists() and not p.is_file():
        return f"Error: '{path}' exists but is not a regular file."
    try:
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(content)
    except Exception as e:
        return f"Error writing: {e}"
    return f"Wrote {len(content)} chars to {path}."


def tool_create_note(title: str, content: str, filename: str = "") -> str:
    """Create a NEW markdown note. Fails if the file already exists."""
    title = (title or "").strip()
    content = content or ""
    if not title and not filename:
        return "Error: provide either a title or a filename."

    if filename:
        name = filename.strip()
        if not name.lower().endswith(".md"):
            name += ".md"
    else:
        # Slugify the title.
        slug = "".join(
            c.lower() if c.isalnum() else "-" for c in title
        ).strip("-")
        while "--" in slug:
            slug = slug.replace("--", "-")
        name = (slug or "note") + ".md"

    p = safe_path(name)
    if p.exists():
        return (
            f"Error: '{name}' already exists. Pick a different filename or "
            "call write_text_file to overwrite."
        )

    body = content.lstrip()
    if title and not body.startswith("# "):
        body = f"# {title}\n\n{body}"
    try:
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(body)
    except Exception as e:
        return f"Error creating note: {e}"
    return f"Created {name} ({len(body)} chars)."


def tool_read_pdf(path: str) -> str:
    from pypdf import PdfReader
    p = safe_path(path)
    if not p.exists():
        return f"Error: '{path}' does not exist."
    if p.suffix.lower() != ".pdf":
        return f"Error: '{path}' is not a PDF."
    try:
        reader = PdfReader(str(p))
        pages = [page.extract_text() or "" for page in reader.pages]
    except Exception as e:
        return f"Error reading PDF: {e}"
    text = "\n\n".join(pages).strip()
    if not text:
        return ("(PDF has no extractable text - it may be a scanned image. "
                "Convert to PNG and use describe_image instead.)")
    if len(text) > MAX_FILE_CHARS:
        text = text[:MAX_FILE_CHARS] + "\n\n...(truncated)"
    return f"[{len(reader.pages)} pages]\n\n{text}"


async def tool_describe_image(path: str, question: str = "") -> str:
    p = safe_path(path)
    if not p.exists():
        return f"Error: '{path}' does not exist."
    if p.suffix.lower() not in IMAGE_EXTS:
        return f"Error: '{path}' is not a supported image ({sorted(IMAGE_EXTS)})."
    if p.stat().st_size > MAX_IMAGE_BYTES:
        return f"Error: image too large ({p.stat().st_size} bytes > {MAX_IMAGE_BYTES})."

    img_b64 = base64.b64encode(p.read_bytes()).decode()
    q = (question or "").strip() or "Describe this image in detail."
    payload = {
        "model": OLLAMA_VISION_MODEL,
        "messages": [{"role": "user", "content": q, "images": [img_b64]}],
        "stream": False,
        # qwen3.5 is a hybrid thinking model; disable the <think> phase so
        # num_predict goes to the actual answer instead of internal reasoning.
        "think": False,
        "options": {"temperature": 0.3, "num_predict": 600},
    }
    async with httpx.AsyncClient(timeout=LLM_TIMEOUT_S) as client:
        r = await client.post(f"{OLLAMA_URL}/api/chat", json=payload)
        if r.status_code != 200:
            return f"Error: vision model {OLLAMA_VISION_MODEL} returned {r.status_code}: {r.text[:200]}"
        data = r.json()
    msg = data.get("message", {}) or {}
    content = (msg.get("content") or "").strip()
    if content:
        return content
    # Fallback: some Ollama builds still emit reasoning into `thinking`
    # even with think=False. Salvage it so the agent sees *something*.
    thinking = (msg.get("thinking") or "").strip()
    if thinking:
        return thinking
    return f"(vision model {OLLAMA_VISION_MODEL} returned empty response; done_reason={data.get('done_reason')})"


# ── Tool schemas (sent to the LLM) ──────────────────────────
TOOLS_SCHEMA: list[dict] = [
    {
        "type": "function",
        "function": {
            "name": "list_directory",
            "description": (
                "List files and subfolders inside the agent's sandbox folder. "
                "Use an empty path to list the root."
            ),
            "parameters": {
                "type": "object",
                "properties": {
                    "path": {
                        "type": "string",
                        "description": "Relative path from sandbox root. Use '' for root.",
                    }
                },
                "required": [],
            },
        },
    },
    {
        "type": "function",
        "function": {
            "name": "read_text_file",
            "description": (
                "Read a text file (.txt, .md, .py, .json, .csv, etc) from the sandbox. "
                "Returned content is truncated if longer than a few thousand chars."
            ),
            "parameters": {
                "type": "object",
                "properties": {"path": {"type": "string"}},
                "required": ["path"],
            },
        },
    },
    {
        "type": "function",
        "function": {
            "name": "write_text_file",
            "description": (
                "Create or overwrite a text file in the sandbox. "
                "Only call this when the user explicitly asks to save, write, or create a file."
            ),
            "parameters": {
                "type": "object",
                "properties": {
                    "path": {"type": "string", "description": "Relative path under the sandbox."},
                    "content": {"type": "string", "description": "Full file contents."},
                },
                "required": ["path", "content"],
            },
        },
    },
    {
        "type": "function",
        "function": {
            "name": "create_note",
            "description": (
                "Create a NEW markdown (.md) note in the sandbox. Use this when the user "
                "asks you to research, summarize, brainstorm, take notes, or otherwise write "
                "a fresh document. The content can be as long and detailed as the topic needs "
                "(many paragraphs, bullet lists, etc). Fails if a file with the same name "
                "already exists."
            ),
            "parameters": {
                "type": "object",
                "properties": {
                    "title": {
                        "type": "string",
                        "description": "Human-readable title; used for the H1 heading and to auto-name the file.",
                    },
                    "content": {
                        "type": "string",
                        "description": (
                            "Full markdown body. Do NOT truncate to fit the voice reply limit; "
                            "that limit only applies to what you say back to the user."
                        ),
                    },
                    "filename": {
                        "type": "string",
                        "description": "Optional filename (with or without .md). Defaults to a slug of the title.",
                    },
                },
                "required": ["title", "content"],
            },
        },
    },
    {
        "type": "function",
        "function": {
            "name": "read_pdf",
            "description": "Extract the text of a PDF file in the sandbox.",
            "parameters": {
                "type": "object",
                "properties": {"path": {"type": "string"}},
                "required": ["path"],
            },
        },
    },
    {
        "type": "function",
        "function": {
            "name": "describe_image",
            "description": (
                "Look at an image (.png, .jpg, .webp, etc) in the sandbox and answer a question about it. "
                "Use this whenever the user asks what is in a picture, photo, screenshot, or image."
            ),
            "parameters": {
                "type": "object",
                "properties": {
                    "path": {"type": "string"},
                    "question": {
                        "type": "string",
                        "description": "What to look for or ask about the image.",
                    },
                },
                "required": ["path"],
            },
        },
    },
]


# ── Agent loop ──────────────────────────────────────────────
SYSTEM_PROMPT_TEMPLATE = (
    "You are Pony P1, a concise voice assistant running on a tiny 160x128 TFT display.\n"
    "You have tools to read files, write files, create new markdown notes, read PDFs, "
    "and describe images inside a single sandbox folder on the user's Mac: {root}\n"
    "\n"
    "When to call tools:\n"
    " - Call a tool when the user asks about files/folders/images/PDFs in that sandbox, "
    "or asks you to save, write, note down, research-and-save, or create a document.\n"
    " - For brand-new markdown notes (research, summaries, brainstorms) prefer create_note.\n"
    " - Use write_text_file to overwrite an existing file or to create non-markdown files.\n"
    " - Never invent file names. If unsure what exists, call list_directory first.\n"
    "\n"
    "Output length rules (IMPORTANT - read carefully):\n"
    " - The 300-character / 3-sentence limit applies ONLY to your final spoken reply to "
    "the user (what you say after all tools have run). It is a display constraint.\n"
    " - File contents passed into write_text_file or create_note have NO length limit. "
    "Write as much detail as the task needs - multiple paragraphs, headings, bullet lists, "
    "etc. Do NOT compress file contents to fit the 300-char rule.\n"
    " - Your final spoken reply should be plain text, no markdown, no emoji, and just "
    "confirm what you did (e.g. 'Saved horses.md with 5 facts.').\n"
)


async def _ollama_chat(messages: list[dict], tools: list[dict] | None) -> dict:
    payload: dict[str, Any] = {
        "model": OLLAMA_MODEL,
        "messages": messages,
        "stream": False,
        # qwen3.5 thinking mode eats the token budget before producing
        # tool_calls or the final answer. Turn it off for the agent loop.
        "think": False,
        # Tool-call arguments can contain a whole markdown document, so give
        # the model a generous budget. 600 tokens was too tight and caused
        # done_reason=length with empty content + no tool_calls emitted.
        "options": {"temperature": 0.6, "num_predict": 4000},
    }
    if tools:
        payload["tools"] = tools
    async with httpx.AsyncClient(timeout=LLM_TIMEOUT_S) as client:
        r = await client.post(f"{OLLAMA_URL}/api/chat", json=payload)
        r.raise_for_status()
        return r.json()


async def _execute_tool(name: str, args: dict) -> str:
    if name == "list_directory":
        return tool_list_directory(args.get("path", ""))
    if name == "read_text_file":
        return tool_read_text_file(args.get("path", ""))
    if name == "write_text_file":
        return tool_write_text_file(args.get("path", ""), args.get("content", ""))
    if name == "create_note":
        return tool_create_note(
            args.get("title", ""),
            args.get("content", ""),
            args.get("filename", ""),
        )
    if name == "read_pdf":
        return tool_read_pdf(args.get("path", ""))
    if name == "describe_image":
        return await tool_describe_image(args.get("path", ""), args.get("question", ""))
    return f"Error: unknown tool '{name}'."


async def run_agent(prompt: str) -> tuple[str, list[dict], float]:
    """Run the tool-calling loop. Returns (final_text, trace, elapsed_s)."""
    _ensure_sandbox()
    t0 = time.time()

    messages: list[dict] = [
        {"role": "system", "content": SYSTEM_PROMPT_TEMPLATE.format(root=AGENT_ROOT)},
        {"role": "user", "content": prompt},
    ]
    trace: list[dict] = []

    for step in range(MAX_ITERATIONS):
        data = await _ollama_chat(messages, tools=TOOLS_SCHEMA)
        msg = data.get("message", {}) or {}
        # Append the model turn even if content is empty — it may contain tool_calls.
        messages.append(msg)

        tool_calls = msg.get("tool_calls") or []
        if not tool_calls:
            return (msg.get("content") or "").strip(), trace, time.time() - t0

        for call in tool_calls:
            fn = call.get("function", {}) or {}
            name = fn.get("name", "")
            raw_args = fn.get("arguments", {}) or {}
            if isinstance(raw_args, str):
                try:
                    args = json.loads(raw_args)
                except Exception:
                    args = {}
            else:
                args = dict(raw_args)

            # Log compactly: truncate long args.
            arg_repr = json.dumps(args, ensure_ascii=False)
            if len(arg_repr) > 200:
                arg_repr = arg_repr[:200] + "...}"
            print(f"  tool[{step + 1}]: {name}({arg_repr})")

            try:
                result = await _execute_tool(name, args)
            except Exception as e:
                result = f"Error: {e}"

            if len(result) > 6000:
                result = result[:6000] + "\n...(truncated)"

            short_result = result.replace("\n", " ")[:140]
            print(f"    -> {short_result}{'...' if len(result) > 140 else ''}")

            trace.append({"tool": name, "args": args, "result_preview": short_result})
            messages.append({
                "role": "tool",
                "name": name,
                "content": result,
            })

    return (
        "(agent hit max tool iterations without producing an answer)",
        trace,
        time.time() - t0,
    )
