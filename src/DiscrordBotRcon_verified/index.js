// DSBOT_V2 — Discord RCON бот Zevvoryn + веб-панель
require('dotenv').config();
const {
    Client, GatewayIntentBits, InteractionType,
    EmbedBuilder, AutocompleteInteraction,
} = require('discord.js');
const { RconBridge, stripColors, asCodeBlock } = require('./rcon');
const { QUICK_CMDS } = require('./commands');

const TOKEN      = process.env.DISCORD_TOKEN;
const ADMIN_ROLE = process.env.ADMIN_ROLE_ID || '';
const CHAN       = process.env.COMMAND_CHANNEL_ID || '';

// ── Веб-панель (WEBPANEL_V1) — работает независимо от Discord-бота ──
let webPanelStarted = false;
if (process.env.WEB_ENABLED === 'true') {
    try {
        require('./webpanel').start();
        webPanelStarted = true;
    } catch (e) {
        console.warn('[WebPanel] Не удалось запустить:', e.message);
        console.warn('[WebPanel] Убедитесь, что express и ws установлены: npm install');
    }
}

if (!TOKEN) {
    console.log('[Bot] DISCORD_TOKEN не задан — Discord-бот пропущен, работает только веб-панель (если включена).');
    if (!webPanelStarted) {
        console.error('[Bot] DISCORD_TOKEN не задан и веб-панель выключена/не запущена — делать нечего, выхожу.');
        process.exit(1);
    }
}

// ── Вспомогательные функции ─────────────────────────────────
function makeRcon() {
    return new RconBridge({
        host:     process.env.RCON_HOST     || '127.0.0.1',
        port:     Number(process.env.RCON_PORT || 25575),
        password: process.env.RCON_PASSWORD || '',
        timeout:  Number(process.env.RCON_TIMEOUT_MS || 10000),
    });
}

// Отправить RCON-команду и получить ответ строкой
async function rcon(cmd) {
    const b = makeRcon();
    try {
        return stripColors(String(await b.send(cmd) || '(empty response)'));
    } finally {
        b.close().catch(() => {});
    }
}

// DSBOT_V2: длинный ответ бьём несколькими сообщениями
async function replyLong(interaction, text) {
    const MAX = 1900;
    const chunks = [];
    for (let i = 0; i < text.length; i += MAX) chunks.push(text.slice(i, i + MAX));
    if (chunks.length === 0) chunks.push('(empty response)');
    await interaction.editReply('```\n' + chunks[0] + '\n```');
    for (let i = 1; i < chunks.length; i++) {
        await interaction.followUp({ content: '```\n' + chunks[i] + '\n```', ephemeral: false });
    }
}

function isAdmin(interaction) {
    if (ADMIN_ROLE) return interaction.member?.roles?.cache?.has(ADMIN_ROLE);
    return interaction.member?.permissions?.has('Administrator');
}

// ── Discord клиент (только если задан токен) ─────────────────
if (TOKEN) {
const client = new Client({
    intents: [GatewayIntentBits.Guilds],
});

client.once('ready', () => {
    console.log(`[Bot] Ready: ${client.user.tag}`);
});

client.on('interactionCreate', async interaction => {
    // ── Autocomplete (/cmd) ──
    if (interaction.type === InteractionType.ApplicationCommandAutocomplete) {
        if (interaction.commandName === 'cmd') {
            const focused = interaction.options.getFocused().toLowerCase();
            const choices = QUICK_CMDS
                .filter(c => c.toLowerCase().includes(focused))
                .slice(0, 25)
                .map(c => ({ name: c, value: c }));
            return interaction.respond(choices);
        }
        return;
    }

    if (interaction.type !== InteractionType.ApplicationCommand) return;

    // Проверка канала
    if (CHAN && interaction.channelId !== CHAN) {
        return interaction.reply({
            content: `Команды только в <#${CHAN}>`,
            ephemeral: true,
        });
    }

    const { commandName } = interaction;
    const admin = isAdmin(interaction);

    // ── /status — не нужно defer ──
    if (commandName === 'status') {
        await interaction.deferReply();
        try {
            const t0 = Date.now();
            const resp = await rcon('list');
            const ms = Date.now() - t0;
            const embed = new EmbedBuilder()
                .setColor(0x3fb950)
                .setTitle('\u2705 Сервер онлайн')
                .setDescription('```' + resp.trim() + '```')
                .setFooter({ text: `RCON ping: ${ms}ms` })
                .setTimestamp();
            return interaction.editReply({ embeds: [embed] });
        } catch (err) {
            const embed = new EmbedBuilder()
                .setColor(0xf85149)
                .setTitle('\u274c Сервер недоступен')
                .setDescription(err.message);
            return interaction.editReply({ embeds: [embed] });
        }
    }

    await interaction.deferReply();

    try {
        // ── /list ──
        if (commandName === 'list') {
            const r = await rcon('list');
            return replyLong(interaction, r);
        }

        // ── /say ──
        if (commandName === 'say') {
            const text = interaction.options.getString('text');
            await rcon(`say ${text}`);
            return interaction.editReply(`\u2705 Отправлено: **${text}**`);
        }

        // ── /cmd — фулл поддержка команд ──
        if (commandName === 'cmd') {
            if (!admin) return interaction.editReply({ content: '\u274c Недостаточно прав.', ephemeral: true });
            const cmd = interaction.options.getString('command');
            const r = await rcon(cmd);
            return replyLong(interaction, r);
        }

        // ── /tp ──
        if (commandName === 'tp') {
            if (!admin) return interaction.editReply({ content: '\u274c Недостаточно прав.', ephemeral: true });
            const player = interaction.options.getString('player');
            const target = interaction.options.getString('target');
            const r = await rcon(`tp ${player} ${target}`);
            return replyLong(interaction, r || '\u2705 OK');
        }

        // ── /give ──
        if (commandName === 'give') {
            if (!admin) return interaction.editReply({ content: '\u274c Недостаточно прав.', ephemeral: true });
            const player = interaction.options.getString('player');
            const item   = interaction.options.getString('item');
            const amount = interaction.options.getInteger('amount') || 1;
            const r = await rcon(`give ${player} ${item} ${amount}`);
            return replyLong(interaction, r || '\u2705 OK');
        }

        // ── /kick ──
        if (commandName === 'kick') {
            if (!admin) return interaction.editReply({ content: '\u274c Недостаточно прав.', ephemeral: true });
            const player = interaction.options.getString('player');
            const reason = interaction.options.getString('reason') || '';
            const r = await rcon(`kick ${player}${reason ? ' ' + reason : ''}`);
            return replyLong(interaction, r || `\u2705 ${player} выкинут`);
        }

        // ── /gamemode ──
        if (commandName === 'gamemode') {
            if (!admin) return interaction.editReply({ content: '\u274c Недостаточно прав.', ephemeral: true });
            const mode   = interaction.options.getString('mode');
            const player = interaction.options.getString('player') || '@a';
            const r = await rcon(`gamemode ${mode} ${player}`);
            return replyLong(interaction, r || '\u2705 OK');
        }

        // ── /weather ──
        if (commandName === 'weather') {
            if (!admin) return interaction.editReply({ content: '\u274c Недостаточно прав.', ephemeral: true });
            const type = interaction.options.getString('type');
            const r = await rcon(`weather ${type}`);
            return replyLong(interaction, r || '\u2705 OK');
        }

        // ── /time ──
        if (commandName === 'time') {
            if (!admin) return interaction.editReply({ content: '\u274c Недостаточно прав.', ephemeral: true });
            const preset = interaction.options.getString('preset');
            const r = await rcon(`time ${preset}`);
            return replyLong(interaction, r || '\u2705 OK');
        }

        // ── /difficulty ──
        if (commandName === 'difficulty') {
            if (!admin) return interaction.editReply({ content: '\u274c Недостаточно прав.', ephemeral: true });
            const level = interaction.options.getString('level');
            const r = await rcon(`difficulty ${level}`);
            return replyLong(interaction, r || '\u2705 OK');
        }

        // ── /stop ──
        if (commandName === 'stop') {
            if (!admin) return interaction.editReply({ content: '\u274c Недостаточно прав.', ephemeral: true });
            await rcon('stop').catch(() => {}); // сервер закроет соединение
            return interaction.editReply('\u2705 Сервер останавливается...');
        }

        // ── /whitelist ──
        if (commandName === 'whitelist') {
            if (!admin) return interaction.editReply({ content: '\u274c Недостаточно прав.', ephemeral: true });
            const action = interaction.options.getString('action');
            const player = interaction.options.getString('player') || '';
            const r = await rcon(`whitelist ${action}${player ? ' ' + player : ''}`);
            return replyLong(interaction, r || '\u2705 OK');
        }

        // ── /save ──
        if (commandName === 'save') {
            if (!admin) return interaction.editReply({ content: '\u274c Недостаточно прав.', ephemeral: true });
            const r = await rcon('save-all');
            return replyLong(interaction, r || '\u2705 Мир сохранён');
        }

        return interaction.editReply('\u2753 Неизвестная команда.');

    } catch (err) {
        const msg = `\u274c RCON ошибка: ${err.message}`;
        try { await interaction.editReply(msg); } catch (_) {}
    }
});

client.login(TOKEN);
} // if (TOKEN)
