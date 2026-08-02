// DSBOT_V2 — полная поддержка команд + помощьники
const { SlashCommandBuilder, EmbedBuilder } = require('discord.js');

// Быстрые команды: пользователь выбирает из списка — не нужно печатать
const QUICK_CMDS = [
    'list', 'save-all', 'help',
    'whitelist list', 'whitelist on', 'whitelist off',
    'weather clear', 'weather rain', 'weather thunder',
    'time set day', 'time set night', 'time set noon',
    'difficulty 0', 'difficulty 1', 'difficulty 2', 'difficulty 3',
    'gamemode survival @a', 'gamemode creative @a',
    'kill @e[type=!player]',
];

const commands = [
    // /list — список игроков
    new SlashCommandBuilder()
        .setName('list')
        .setDescription('Список игроков на сервере'),

    // /say — сообщение через сервер
    new SlashCommandBuilder()
        .setName('say')
        .setDescription('Отправить сообщение всем игрокам')
        .addStringOption(o => o.setName('text').setDescription('Текст').setRequired(true)),

    // /cmd — произвольная RCON-команда
    new SlashCommandBuilder()
        .setName('cmd')
        .setDescription('RCON: любая команда (только admins)')
        .addStringOption(o => o
            .setName('command')
            .setDescription('Любая команда сервера')
            .setRequired(true)
            .setAutocomplete(true)),

    // /tp — телепорт
    new SlashCommandBuilder()
        .setName('tp')
        .setDescription('Телепорт игрока (только admins)')
        .addStringOption(o => o.setName('player').setDescription('Имя игрока (или @a/@p)').setRequired(true))
        .addStringOption(o => o.setName('target').setDescription('Цель: другой игрок или "x y z"').setRequired(true)),

    // /give — выдать предмет
    new SlashCommandBuilder()
        .setName('give')
        .setDescription('Выдать предмет (только admins)')
        .addStringOption(o => o.setName('player').setDescription('Игрок').setRequired(true))
        .addStringOption(o => o.setName('item').setDescription('Предмет (minecraft:diamond)').setRequired(true))
        .addIntegerOption(o => o.setName('amount').setDescription('Количество (1-64)').setMinValue(1).setMaxValue(64)),

    // /kick — выгнать
    new SlashCommandBuilder()
        .setName('kick')
        .setDescription('Выгнать игрока (только admins)')
        .addStringOption(o => o.setName('player').setDescription('Игрок').setRequired(true))
        .addStringOption(o => o.setName('reason').setDescription('Причина')),

    // /gamemode — режим
    new SlashCommandBuilder()
        .setName('gamemode')
        .setDescription('Изменить режим игррока (только admins)')
        .addStringOption(o => o
            .setName('mode')
            .setDescription('Режим')
            .setRequired(true)
            .addChoices(
                { name: 'survival', value: 'survival' },
                { name: 'creative', value: 'creative' },
                { name: 'adventure', value: 'adventure' },
                { name: 'spectator', value: 'spectator' },
            ))
        .addStringOption(o => o.setName('player').setDescription('Игрок (по умолчанию @a)')),

    // /weather — погода
    new SlashCommandBuilder()
        .setName('weather')
        .setDescription('Изменить погоду (только admins)')
        .addStringOption(o => o
            .setName('type')
            .setDescription('Тип погоды')
            .setRequired(true)
            .addChoices(
                { name: 'clear', value: 'clear' },
                { name: 'rain', value: 'rain' },
                { name: 'thunder', value: 'thunder' },
            )),

    // /time — время
    new SlashCommandBuilder()
        .setName('time')
        .setDescription('Установить время (только admins)')
        .addStringOption(o => o
            .setName('preset')
            .setDescription('Время дня')
            .setRequired(true)
            .addChoices(
                { name: 'day (1000)',   value: 'set 1000'  },
                { name: 'noon (6000)',  value: 'set 6000'  },
                { name: 'night (13000)',value: 'set 13000' },
                { name: 'midnight (18000)', value: 'set 18000' },
            )),

    // /difficulty — сложность
    new SlashCommandBuilder()
        .setName('difficulty')
        .setDescription('Изменить сложность (только admins)')
        .addStringOption(o => o
            .setName('level')
            .setDescription('Сложность')
            .setRequired(true)
            .addChoices(
                { name: '0 — peaceful', value: '0' },
                { name: '1 — easy',     value: '1' },
                { name: '2 — normal',   value: '2' },
                { name: '3 — hard',     value: '3' },
            )),

    // /stop — остановка сервера
    new SlashCommandBuilder()
        .setName('stop')
        .setDescription('Остановить сервер (только admins)'),

    // /whitelist — белый список
    new SlashCommandBuilder()
        .setName('whitelist')
        .setDescription('Белый список (только admins)')
        .addStringOption(o => o
            .setName('action')
            .setDescription('Действие')
            .setRequired(true)
            .addChoices(
                { name: 'list',   value: 'list'   },
                { name: 'on',     value: 'on'     },
                { name: 'off',    value: 'off'    },
                { name: 'add',    value: 'add'    },
                { name: 'remove', value: 'remove' },
                { name: 'reload', value: 'reload' },
            ))
        .addStringOption(o => o.setName('player').setDescription('Ник (для add/remove)')),

    // /save — сохранение
    new SlashCommandBuilder()
        .setName('save')
        .setDescription('Сохранить мир (только admins)'),

    // /status — статус сервера (RCON ping)
    new SlashCommandBuilder()
        .setName('status')
        .setDescription('Статус сервера (RCON ping)'),

].map(c => c.toJSON());

const commandMap = new Map();
for (const c of commands) commandMap.set(c.name, c);
const commandData = commands;
const { stripColors } = require('./rcon');
module.exports = { commands, commandData, commandMap, stripColors, QUICK_CMDS };
