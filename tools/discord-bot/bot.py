"""Squadala! bug-report bot.

One slash command, /bugreport, in one server (the dev channel's server). A report is
saved under coord-multiplayer/player-logs/<user>-<UTC time>/, added to
coord-multiplayer/reports/player-reports.md as 'NEW (bot)' and posted to the
private dev channel. Everything the player types is stored and shown as quoted,
untrusted text.

No privileged intents and no message content. Besides /bugreport, the bot gets
message events (without their text) only to remove plain messages typed in the
public report channel; it logs the author id and the time, nothing else.
Runs on the owner's PC (run-bot.ps1).

Optionally (config "intake", README "Problem-report intake") it also takes the kits' masked problem reports on a
small HTTP endpoint on 127.0.0.1, which the owner puts behind an HTTPS tunnel or proxy: each upload is checked
(intake_core.py), saved, filed as 'NEW (auto)' and posted to the dev channel like a /bugreport.

Usage: python bot.py --config <path to discord-bot.json>
"""

import argparse
import asyncio
import datetime as dt
import io
import logging
import logging.handlers
import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import report_core as core  # noqa: E402
import intake_core as intake  # noqa: E402

try:
    import discord
    from discord import app_commands
except ImportError:  # pragma: no cover - the tests never import this file
    print("discord.py is not installed; see tools/discord-bot/README.md", file=sys.stderr)
    sys.exit(3)

log = logging.getLogger("report-bot")

GENERIC_ERROR = "Sorry, something went wrong with your report. Please try again later."


def setup_logging(log_path, token):
    fmt = "%(asctime)s %(levelname)s %(name)s: %(message)s"
    handler = logging.handlers.RotatingFileHandler(log_path, maxBytes=1_000_000, backupCount=3, encoding="utf-8")
    handler.setFormatter(core.ScrubFormatter(token, fmt))
    console = logging.StreamHandler()
    console.setFormatter(core.ScrubFormatter(token, fmt))
    root = logging.getLogger()
    root.setLevel(logging.INFO)
    root.addHandler(handler)
    root.addHandler(console)


class ReportBot(discord.Client):
    def __init__(self, cfg):
        intents = discord.Intents.none()
        intents.guilds = True  # channel and role cache; not privileged
        intents.guild_messages = True  # message events without text (no Message Content intent)
        super().__init__(intents=intents, allowed_mentions=discord.AllowedMentions.none(),
                         application_id=cfg.get("application_id"))
        self.cfg = cfg
        self.guild_id = None  # the one server served; set in setup_hook
        self.tree = app_commands.CommandTree(self)
        self.tree.on_error = self.on_command_error
        self.user_limit = core.RateLimiter(core.USER_LIMIT, core.USER_WINDOW)
        self.global_limit = core.RateLimiter(core.GLOBAL_LIMIT, core.GLOBAL_WINDOW)
        self.lock = asyncio.Lock()
        self.notice_limit = core.RateLimiter(1, core.NOTICE_WINDOW)
        self.delete_forbidden_logged = False
        self.logs_dir = cfg["coord_dir"] / "player-logs"
        self.tracker = cfg["coord_dir"] / "reports" / core.TRACKER_NAME
        self.intake_cfg = cfg.get("intake")
        self.intake_limits = intake.Limits()
        self.intake_seen = intake.SeenIds(self.logs_dir / "auto-intake.json")
        self.intake_runner = None
        self.heartbeat_task = None

    async def setup_hook(self):
        # The server is the dev channel's server (the config's guild_id, if set, must match).
        # Fail closed: without it the bot stops; it never registers /bugreport globally.
        # Raises core.StartupError (caught in main, exit 2) when the server is unknown or the
        # report channel is on another server.
        self.guild_id = core.resolve_guild(self.cfg["guild_id"], await self._channel_guild("dev_channel_id"))
        if self.cfg["report_channel_id"] is not None:
            core.check_report_channel(self.guild_id, self.cfg["report_channel_id"],
                                      await self._channel_guild("report_channel_id"))
        guild = discord.Object(id=self.guild_id)
        self._add_commands(guild)
        synced = await self.tree.sync(guild=guild)
        log.info("commands synced to server %s: %s", self.guild_id, ", ".join(c.name for c in synced))
        if self.intake_cfg:
            await self.start_intake()

    async def start_intake(self):
        """The problem-report endpoint (intake_core.PATH) on the configured address (127.0.0.1 by default)."""
        from aiohttp import web  # discord.py's own dependency

        app = web.Application(client_max_size=intake.MAX_UPLOAD_BYTES + 1024)
        app.router.add_route("*", "/{tail:.*}", self.handle_intake)
        self.intake_runner = web.AppRunner(app, access_log=None)  # no access log: no addresses on disk
        await self.intake_runner.setup()
        site = web.TCPSite(self.intake_runner, self.intake_cfg["host"], self.intake_cfg["port"])
        await site.start()
        log.info("problem-report intake listening on %s:%d%s", self.intake_cfg["host"], self.intake_cfg["port"],
                 intake.PATH)

    async def close(self):
        if self.intake_runner is not None:
            await self.intake_runner.cleanup()
        await super().close()

    async def handle_intake(self, request):
        """One upload: headers, rate, body, zip, then saved and filed (intake_core). The answer is JSON; a refusal
        says why in a few words. Addresses are used for the rate limit in memory only, never logged or stored."""
        from aiohttp import web

        def answer(status, **body):
            headers = {"Cache-Control": "no-store"}
            if body.get("retry_after"):
                headers["Retry-After"] = str(body["retry_after"])
            return web.json_response(body, status=status, headers=headers)

        taken = None
        try:
            install, report_id = intake.check_headers(
                request.method, request.path, request.headers.get("Content-Type"),
                request.headers.get("Content-Length"), request.headers.get("X-Squadala-Install"),
                request.headers.get("X-Squadala-Report"))
            if report_id in self.intake_seen:
                return answer(200, ok=True, duplicate=True)
            address = intake.sender_address(self.intake_cfg.get("address_header"), request.headers, request.remote)
            taken = self.intake_limits.acquire(install, address)
            try:
                data = await asyncio.wait_for(request.read(), timeout=20)
            except asyncio.TimeoutError:
                raise intake.Refused(408, "too slow")
            except web.HTTPRequestEntityTooLarge:
                raise intake.Refused(413, "too big")
            if len(data) != int(request.headers.get("Content-Length")):
                raise intake.Refused(400, "body length differs from Content-Length")
            files = await asyncio.to_thread(intake.check_zip, data)
            when = dt.datetime.now(dt.timezone.utc)
            async with self.lock:
                if report_id in self.intake_seen:
                    intake.Limits.release(taken)
                    return answer(200, ok=True, duplicate=True)
                if core.over_budget(core.stored_size(self.logs_dir), len(data)):
                    raise intake.Refused(503, "the report box is full; try again later", retry_after=3600)
                number, folder = intake.save_upload(self.logs_dir, self.tracker, when, install, report_id, data, files)
                self.intake_seen.add(report_id)
        except intake.Refused as e:
            if taken and e.status != 429:
                intake.Limits.release(taken)
            if e.status in (404, 405):
                pass
            elif intake.is_health_check(request.headers.get("Content-Length"), request.headers.get("X-Squadala-Install"),
                                        request.headers.get("X-Squadala-Report")):
                log.info("problem-report intake check (an empty POST, no ids): answered %d", e.status)
            else:
                log.info("problem-report upload refused (%d: %s)", e.status, e.reason)
            return answer(e.status, ok=False, error=e.reason, retry_after=e.retry_after)
        except OSError:
            if taken:
                intake.Limits.release(taken)
            log.exception("saving a problem-report upload failed")
            return answer(500, ok=False, error="could not save; try again later")
        rel = f"player-logs/{folder.name}"
        log.info("auto report #%d from install %s saved to %s (%d bytes)", number, intake.install_tag(install), rel,
                 len(data))
        await self.post_auto_to_dev(number, when, install, files, data, rel)
        return answer(200, ok=True, report=number)

    async def post_auto_to_dev(self, number, when, install, files, data, rel):
        """As post_to_dev: the upload's problem lines in a code block under the untrusted label, and the zip."""
        try:
            channel = self.get_channel(self.cfg["dev_channel_id"]) or await self.fetch_channel(self.cfg["dev_channel_id"])
            body = f"{core.UNTRUSTED_LABEL}:\n```text\n{intake.dev_post_text(files)}\n```"
            embed = discord.Embed(title=f"Auto problem report #{number}", description=body, timestamp=when,
                                  colour=discord.Colour.dark_orange())
            embed.add_field(name="Install", value=f"`auto-{intake.install_tag(install)}` (a random id)")
            embed.add_field(name="Kit version", value=intake.report_version(files["problem-report.txt"]) or "?")
            embed.add_field(name="Saved", value=f"`{rel}`", inline=False)
            await channel.send(embed=embed, files=[discord.File(io.BytesIO(data), filename=f"report-{number}-auto.zip")])
        except discord.HTTPException as e:
            # The report is already saved; the dev post is a copy.
            log.error("posting auto report #%d to the dev channel failed (%s)", number, type(e).__name__)

    async def _channel_guild(self, key):
        """The server id of a configured channel, or None if the bot cannot read it."""
        try:
            channel = await self.fetch_channel(self.cfg[key])
        except discord.HTTPException as e:
            log.error("cannot read the %s channel %s (%s)", key.split("_")[0], self.cfg[key], type(e).__name__)
            return None
        return getattr(channel, "guild_id", None) or getattr(getattr(channel, "guild", None), "id", None)

    async def on_ready(self):
        log.info("online as %s", self.user)
        if self.heartbeat_task is None:
            self.heartbeat_task = asyncio.ensure_future(self._heartbeat_loop())
        self._check_report_channel_rights()

    async def _heartbeat_loop(self):
        """Every 5 minutes while the gateway is connected: the UTC time into player-logs/discord-bot.heartbeat.
        The coordinator watchdog logs 'discord bot down' when the file is over 30 minutes old."""
        path = self.logs_dir / core.HEARTBEAT_NAME
        while not self.is_closed():
            if self.is_ready():
                try:
                    core.write_heartbeat(path, dt.datetime.now(dt.timezone.utc))
                except OSError as e:
                    log.warning("heartbeat write failed (%s)", type(e).__name__)
            await asyncio.sleep(core.HEARTBEAT_EVERY_SECONDS)

    def _check_report_channel_rights(self):
        """Log once at start when the bot cannot remove messages in the report channel."""
        channel_id = self.cfg["report_channel_id"]
        channel = self.get_channel(channel_id) if channel_id is not None else None
        me = getattr(getattr(channel, "guild", None), "me", None)
        if channel is None or me is None:
            return
        if not channel.permissions_for(me).manage_messages:
            log.error(core.MANAGE_MESSAGES_HELP.format(channel=channel_id))
            self.delete_forbidden_logged = True

    async def on_message(self, message):
        # Owner 2026-11-17: the report channel takes /bugreport only; plain messages are removed.
        if message.guild is None or self.user is None:
            return
        plain = message.type in (discord.MessageType.default, discord.MessageType.reply)

        def remove(can_manage):
            return core.should_remove(message.channel.id, self.cfg["report_channel_id"], message.author.id,
                                      self.user.id, plain, message.webhook_id is not None, can_manage)

        if not remove(False):  # channel, author, type and webhook first; no API call for these
            return
        author = message.author
        if not isinstance(author, discord.Member):
            try:
                author = await message.guild.fetch_member(author.id)
            except discord.HTTPException:
                return  # cannot tell whether they moderate: keep the message
        if not remove(message.channel.permissions_for(author).manage_messages):
            return
        try:
            await message.delete()
        except discord.NotFound:
            return  # already gone
        except discord.Forbidden:
            if not self.delete_forbidden_logged:
                log.error(core.MANAGE_MESSAGES_HELP.format(channel=message.channel.id))
                self.delete_forbidden_logged = True
            return
        except discord.HTTPException as e:
            log.warning("removing a plain message from user %s failed (%s)", author.id, type(e).__name__)
            return
        self.delete_forbidden_logged = False
        log.info("removed a plain message from user %s in the report channel", author.id)
        if author.bot or self.notice_limit.acquire(author.id) > 0:
            return
        await self._tell_removed(message.channel, author)

    async def _tell_removed(self, channel, author):
        """DM the one-line notice; if their DMs are closed, a short mention in the channel that
        deletes itself."""
        try:
            await author.send(core.REMOVED_NOTICE)
            return
        except discord.HTTPException:
            pass
        try:
            await channel.send(f"<@{author.id}> {core.REMOVED_NOTICE}", delete_after=20,
                               allowed_mentions=discord.AllowedMentions(users=[author]))
        except discord.HTTPException as e:
            log.warning("could not tell user %s why their message was removed (%s)", author.id, type(e).__name__)

    async def on_command_error(self, interaction, error):
        # Never show a traceback or error text to the player; it goes to the log only.
        log.error("command failed", exc_info=error)
        try:
            if interaction.response.is_done():
                await interaction.followup.send(GENERIC_ERROR, ephemeral=True)
            else:
                await interaction.response.send_message(GENERIC_ERROR, ephemeral=True)
        except discord.HTTPException:
            pass

    def _add_commands(self, guild):
        @app_commands.command(name="bugreport", description="Send a bug report (and your logs zip) to the Squadala! team.")
        @app_commands.describe(
            description="What happened, and what you were doing (solo or co-op, host or guest).",
            logs="Optional: the zip from the kit's collect-logs (zip only, 25 MB max).",
            version="Optional: your kit version, e.g. 0.11.3.",
        )
        @app_commands.guild_only()
        async def report(
            interaction: discord.Interaction,
            description: app_commands.Range[str, 5, core.MAX_DESCRIPTION],
            logs: discord.Attachment = None,
            version: app_commands.Range[str, 1, 32] = None,
        ):
            await self.handle_report(interaction, description, logs, version)

        self.tree.add_command(report, guild=guild)

    async def refuse(self, interaction, text):
        if interaction.response.is_done():
            await interaction.followup.send(text, ephemeral=True)
        else:
            await interaction.response.send_message(text, ephemeral=True)

    async def handle_report(self, interaction, description, logs, version):
        user = interaction.user
        # Only the owner's server, even if the bot is ever added to another one.
        if not core.guild_allowed(interaction.guild_id, self.guild_id):
            log.warning("refused /bugreport from server %s (not ours)", interaction.guild_id)
            await self.refuse(interaction, "This bot only takes reports in the Squadala! server.")
            return
        allowed = self.cfg["report_channel_id"]
        if not core.channel_allowed(interaction.channel_id, allowed, self.cfg["dev_channel_id"]):
            await self.refuse(interaction, f"Please use /bugreport in <#{allowed}>.")
            return
        if logs is not None:
            problem = core.check_attachment_meta(logs.filename, logs.size)
            if problem:
                await self.refuse(interaction, problem)
                return

        # Rate limits: taken now, before any await, so reports sent at once cannot all pass.
        wait = self.user_limit.acquire(user.id)
        if wait > 0:
            await self.refuse(interaction,
                              f"You sent several reports recently; please try again in {int(wait // 60) + 1} min.")
            return
        if self.global_limit.acquire("all") > 0:
            self.user_limit.release(user.id)
            log.warning("global report limit reached; refused a report from %s", user.name)
            await self.refuse(interaction, "Many reports came in this hour; please try again later.")
            return
        ok = False
        try:
            ok = await self._accept(interaction, user, description, logs, version)
        finally:
            if not ok:
                self.user_limit.release(user.id)
                self.global_limit.release("all")

    async def _accept(self, interaction, user, description, logs, version):
        incoming = logs.size if logs is not None else 0
        if core.over_budget(await asyncio.to_thread(core.stored_size, self.logs_dir), incoming):
            log.warning("player-logs is over its %d MB budget; refused a report from %s",
                        core.LOGS_BUDGET_BYTES // (1024 * 1024), user.name)
            await self.refuse(interaction, "The report box is full right now; please try again later.")
            return False

        await interaction.response.defer(ephemeral=True, thinking=True)
        data = None
        if logs is not None:
            try:
                data = await logs.read()
            except discord.HTTPException as e:
                log.warning("download of the attachment failed (%s)", type(e).__name__)
                await self.refuse(interaction, "I could not download the zip; please try again.")
                return False
            problem = core.check_zip_bytes(data)
            if problem:
                await self.refuse(interaction, problem)
                return False

        text = core.clean_description(description)
        when = dt.datetime.now(dt.timezone.utc)
        username = user.name
        display = getattr(user, "display_name", None) or username
        try:
            async with self.lock:
                if core.over_budget(core.stored_size(self.logs_dir), len(data) if data is not None else 0):
                    await self.refuse(interaction, "The report box is full right now; please try again later.")
                    return False
                number, folder = core.save_report(self.logs_dir, self.tracker, when, username, display, version,
                                                  logs.filename if logs is not None else None, data, text)
        except OSError:
            log.exception("saving a report from %s failed", username)
            await self.refuse(interaction, GENERIC_ERROR)
            return False
        rel = f"player-logs/{folder.name}"
        log.info("report #%d from %s saved to %s (%s)", number, username, rel,
                 f"{len(data)} bytes" if data is not None else "no zip")

        await self.post_to_dev(number, when, user, display, version, text, data, rel)
        await interaction.followup.send(f"Thanks, report #{number} received.", ephemeral=True)
        return True

    async def post_to_dev(self, number, when, user, display, version, text, data, rel):
        try:
            channel = self.get_channel(self.cfg["dev_channel_id"]) or await self.fetch_channel(self.cfg["dev_channel_id"])
            # Player text goes in a code block: no links, images, mentions or formatting render.
            body = f"{core.UNTRUSTED_LABEL}:\n```text\n{core.no_backticks(text)}\n```"
            embed = discord.Embed(title=f"Bug report #{number}", description=body, timestamp=when,
                                  colour=discord.Colour.orange())
            shown = core.no_backticks(" ".join((display or "").split()))[:64]
            embed.add_field(name="Player", value=f"`{shown}` (@{core.clean_handle(user.name)})")
            embed.add_field(name="Kit version", value=core.clean_version(version) or "?")
            embed.add_field(name="Saved", value=f"`{rel}`", inline=False)
            files = []
            if data is not None:
                limit = getattr(getattr(channel, "guild", None), "filesize_limit", 10 * 1024 * 1024)
                if len(data) <= limit:
                    files.append(discord.File(io.BytesIO(data), filename=f"report-{number}-logs.zip"))
                else:
                    embed.add_field(name="Logs", value="Zip too big to re-post here; saved on the PC.", inline=False)
            else:
                embed.add_field(name="Logs", value="None attached.", inline=False)
            await channel.send(embed=embed, files=files)
        except discord.HTTPException as e:
            # The report is already saved; the dev post is a copy.
            log.error("posting report #%d to the dev channel failed (%s)", number, type(e).__name__)


def main():
    here = Path(__file__).resolve().parent
    default_cfg = here.parent.parent.parent / "coord-multiplayer" / "secrets" / "discord-bot.json"
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--config", default=os.environ.get("DISCORD_BOT_CONFIG", str(default_cfg)))
    args = ap.parse_args()
    try:
        cfg = core.load_config(args.config)
        cfg["intake"] = intake.load_intake_config(cfg.pop("intake_raw", None))
    except core.ConfigError as e:
        print(f"discord-bot: {e}", file=sys.stderr)
        return 2
    cfg["coord_dir"].joinpath("player-logs").mkdir(parents=True, exist_ok=True)
    setup_logging(cfg["coord_dir"] / "player-logs" / "discord-bot.log", cfg["token"])
    bot = ReportBot(cfg)
    try:
        bot.run(cfg["token"], log_handler=None)
    except discord.LoginFailure:
        log.error("Discord refused the token (check secrets/discord-bot.json)")
        return 2
    except core.StartupError as e:
        log.error("%s", e)
        return 2  # run-bot.ps1 does not restart on 2: fix the setup first
    return 0


if __name__ == "__main__":
    sys.exit(main())
