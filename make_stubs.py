import os

files = [
    "src/channels/cli.c","src/channels/discord.c","src/channels/slack.c",
    "src/channels/whatsapp.c","src/channels/matrix.c","src/channels/irc.c",
    "src/channels/imessage.c","src/channels/email.c","src/channels/lark.c",
    "src/channels/dingtalk.c","src/channels/qq.c",
    "src/memory/markdown_mem.c","src/memory/none_mem.c","src/memory/chunker.c",
    "src/security/policy.c","src/security/audit.c",
    "src/security/sandbox.c","src/security/tracker.c",
    "src/tools/file_read.c","src/tools/file_write.c",
    "src/tools/file_edit.c","src/tools/file_append.c",
    "src/tools/memory_store.c","src/tools/memory_recall.c","src/tools/memory_forget.c",
    "src/tools/browser.c","src/tools/browser_open.c","src/tools/image.c",
    "src/tools/web_search.c","src/tools/web_fetch.c",
    "src/tools/delegate.c","src/tools/spawn.c",
    "src/tools/composio.c","src/tools/screenshot.c","src/tools/hardware_info.c",
    "src/tools/cron_add.c","src/tools/cron_list.c",
    "src/tools/cron_remove.c","src/tools/cron_run.c",
]

for f in files:
    os.makedirs(os.path.dirname(f), exist_ok=True)
    open(f, "w").write("/* stub */\n")
    print("ok:", f)

print("all done")
