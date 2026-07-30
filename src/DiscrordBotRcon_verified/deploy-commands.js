'use strict';
// DSBOT_V1: регистрация slash-команд в одной гильдии (появляются мгновенно).
// Запуск: npm run deploy

require('dotenv').config();
const { REST, Routes } = require('discord.js');
const { commandData } = require('./commands');

const token = process.env.DISCORD_TOKEN;
const clientId = process.env.CLIENT_ID;
const guildId = process.env.GUILD_ID;

if (!token || !clientId || !guildId) {
	console.error('[deploy] Заполните DISCORD_TOKEN, CLIENT_ID и GUILD_ID в .env');
	process.exit(1);
}

(async () => {
	try {
		const rest = new REST({ version: '10' }).setToken(token);
		const body = commandData.map((c) => c.toJSON());
		await rest.put(Routes.applicationGuildCommands(clientId, guildId), { body });
		console.log('[deploy] Зарегистрировано команд: ' + body.length);
	} catch (err) {
		console.error('[deploy] Ошибка:', err);
		process.exit(1);
	}
})();
