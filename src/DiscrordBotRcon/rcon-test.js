// ============================================================
// RCON_TEST_V1 — самопроверка RCON без Discord.
//
// Запуск:  npm run rcon-test
//         npm run rcon-test -- "say привет"
//
// Что делает:
//   1) читает .env (RCON_HOST/PORT/PASSWORD);
//   2) проверяет, что порт вообще отвечает (голый TCP-коннект);
//   3) логинится паролем;
//   4) шлёт команды help и list и печатает ответы;
//   5) проверяет, что неверный пароль НЕ пускает.
//
// Код возврата 0 — всё хорошо, 1 — есть проваленный шаг.
// ============================================================

require("dotenv").config()

const net = require("net")
const { RconBridge, stripColors } = require("./rcon")

const HOST = process.env.RCON_HOST || "127.0.0.1"
const PORT = Number(process.env.RCON_PORT || 25575)
const PASSWORD = process.env.RCON_PASSWORD || ""
const TIMEOUT = Number(process.env.RCON_TIMEOUT_MS || 10000)

const EXTRA = process.argv.slice(2).join(" ").trim()

let failed = 0

function ok(step, detail) {
	console.log(`  [ OK ] ${step}${detail ? " — " + detail : ""}`)
}

function bad(step, detail) {
	failed++
	console.log(`  [FAIL] ${step}${detail ? " — " + detail : ""}`)
}

function info(text) {
	console.log(text)
}

// Голый TCP-коннект: отличает "сервер не запущен / enable-rcon=false"
// от "пароль не подошёл".
function probePort() {
	return new Promise((resolve) => {
		const socket = new net.Socket()
		const done = (result) => {
			socket.removeAllListeners()
			socket.destroy()
			resolve(result)
		}
		socket.setTimeout(Math.min(TIMEOUT, 5000))
		socket.once("connect", () => done({ open: true }))
		socket.once("timeout", () => done({ open: false, reason: "таймаут подключения" }))
		socket.once("error", (err) => done({ open: false, reason: err.code || err.message }))
		socket.connect(PORT, HOST)
	})
}

async function main() {
	info("")
	info(`RCON_TEST_V1: проверка ${HOST}:${PORT}`)
	info("")

	// Шаг 1 — конфиг
	info("1) Настройки из .env")
	if (!PASSWORD) {
		bad("RCON_PASSWORD", "пустой — сервер с пустым паролем RCON не запускает")
	} else {
		ok("RCON_PASSWORD", `задан (${PASSWORD.length} симв.)`)
	}
	if (!Number.isInteger(PORT) || PORT < 1 || PORT > 65535) bad("RCON_PORT", `странное значение ${PORT}`)
	else ok("RCON_PORT", String(PORT))

	// Шаг 2 — порт
	info("")
	info("2) Порт отвечает?")
	const probe = await probePort()
	if (!probe.open) {
		bad("TCP-подключение", probe.reason)
		info("")
		info("   Подсказка: в консоли сервера выполните `rcon status`.")
		info("   Если выключен: config set enable-rcon true, config set rcon.password <пароль>, config save, rcon start.")
		summary()
		return
	}
	ok("TCP-подключение", "порт открыт")

	// Шаг 3 — логин и команды
	info("")
	info("3) Авторизация и команды")
	const bridge = new RconBridge({ host: HOST, port: PORT, password: PASSWORD, timeout: TIMEOUT })
	try {
		const commands = ["help", "list"]
		if (EXTRA) commands.push(EXTRA)
		let first = true
		for (const command of commands) {
			const answer = await bridge.send(command)
			if (first) {
				ok("Пароль принят")
				first = false
			}
			const text = stripColors(String(answer || "")).trim()
			if (!text) bad(`Команда "${command}"`, "пустой ответ")
			else {
				ok(`Команда "${command}"`, `${text.length} симв.`)
				for (const line of text.split("\n").slice(0, 6)) info("         | " + line)
			}
		}
	} catch (err) {
		bad("Авторизация/команда", err.message)
		info("   Подсказка: пароль в .env должен совпадать с rcon.password в settings.properties.")
	} finally {
		try { await bridge.close() } catch (_) {}
	}

	// Шаг 4 — неверный пароль должен отлетать
	info("")
	info("4) Неверный пароль отклоняется?")
	const wrong = new RconBridge({
		host: HOST,
		port: PORT,
		password: PASSWORD + "_zevvoryn_wrong",
		timeout: TIMEOUT,
	})
	try {
		await wrong.send("list")
		bad("Защита паролем", "сервер пустил с неверным паролем!")
	} catch (_) {
		ok("Защита паролем", "чужой пароль отклонён")
	} finally {
		try { await wrong.close() } catch (_) {}
	}

	summary()
}

function summary() {
	info("")
	if (failed === 0) info("Итог: RCON работает, можно запускать бота (npm start).")
	else info(`Итог: проваленных шагов — ${failed}. Смотрите [FAIL] выше.`)
	info("")
	process.exit(failed === 0 ? 0 : 1)
}

main().catch((err) => {
	console.error("RCON_TEST_V1: неожиданная ошибка:", err)
	process.exit(1)
})
