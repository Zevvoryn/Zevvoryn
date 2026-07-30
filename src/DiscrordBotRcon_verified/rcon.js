'use strict';
// DSBOT_V1: тонкая обёртка над rcon-client.
// Сервер Zevvoryn исполняет команды на тик-потоке и отвечает одним пакетом,
// поэтому держим одно соединение и шлём команды строго по очереди.

const { Rcon } = require('rcon-client');

class RconBridge {
	constructor({ host, port, password, timeout = 10000 }) {
		this.host = host;
		this.port = port;
		this.password = password;
		this.timeout = timeout;
		this.client = null;
		this.connecting = null;
		// Очередь: две команды одновременно по одному сокету — путаница ответов.
		this.queue = Promise.resolve();
	}

	async connect() {
		if (this.client) return this.client;
		if (this.connecting) return this.connecting;

		this.connecting = (async () => {
			const client = await Rcon.connect({
				host: this.host,
				port: this.port,
				password: this.password,
				timeout: this.timeout,
			});
			client.on('error', () => { this.client = null; });
			client.on('end', () => { this.client = null; });
			this.client = client;
			this.connecting = null;
			return client;
		})().catch((err) => {
			this.connecting = null;
			throw err;
		});

		return this.connecting;
	}

	// Одна попытка переподключения: сервер мог быть перезапущен.
	async send(command) {
		const run = async () => {
			try {
				const client = await this.connect();
				return await client.send(command);
			} catch (err) {
				this.client = null;
				const client = await this.connect();
				return await client.send(command);
			}
		};

		const task = this.queue.then(run, run);
		// Очередь не должна рваться из-за одной упавшей команды.
		this.queue = task.then(() => undefined, () => undefined);
		return task;
	}

	async close() {
		const client = this.client;
		this.client = null;
		if (client) {
			try { await client.end(); } catch (err) { /* уже закрыт */ }
		}
	}
}

// Убираем цветовые коды Minecraft, чтобы в Discord был чистый текст.
function stripColors(text) {
	return String(text === null || text === undefined ? '' : text).replace(/\u00a7[0-9a-fk-orA-FK-OR]/g, '');
}

// Discord не принимает больше 2000 символов в сообщении.
function asCodeBlock(text, limit = 1800) {
	const clean = stripColors(text).trimEnd();
	if (!clean) return '```\n(пустой ответ)\n```';
	const cut = clean.length > limit ? clean.slice(0, limit) + '\n... (обрезано)' : clean;
	return '```\n' + cut.split('```').join('`​``') + '\n```';
}

module.exports = { RconBridge, stripColors, asCodeBlock };
