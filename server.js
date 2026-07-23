import express from "express";
import fs from "node:fs/promises";
import path from "node:path";
import { z } from "zod";

import { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { SSEServerTransport } from "@modelcontextprotocol/sdk/server/sse.js";
import {
  StreamableHTTPServerTransport,
} from "@modelcontextprotocol/sdk/server/streamableHttp.js";

const app = express();

const PORT = Number(process.env.PORT ?? 3000);
const ALLOWED_DIRECTORY =
  process.env.ALLOWED_DIRECTORY ??
  String.raw`C:\Users\aaav9\Desktop\TestC++`;

// Если MCP_TOKEN не задан, сервер будет работать без авторизации.
// Для публичного туннеля лучше обязательно задать токен.
const MCP_TOKEN = process.env.MCP_TOKEN;

app.use(express.json());

// Авторизация для MCP-маршрутов
app.use(["/mcp", "/sse", "/messages"], (req, res, next) => {
  if (!MCP_TOKEN) {
    return next();
  }

  const expected = `Bearer ${MCP_TOKEN}`;

  if (req.headers.authorization !== expected) {
    return res.status(401).json({
      error: "Unauthorized",
    });
  }

  next();
});

function safePath(relativePath) {
  const root = path.resolve(ALLOWED_DIRECTORY);
  const target = path.resolve(root, relativePath);
  const relative = path.relative(root, target);

  if (relative === ".." || relative.startsWith(`..${path.sep}`) || path.isAbsolute(relative)) {
    throw new Error("Доступ за пределы разрешённой папки запрещён.");
  }

  return { root, target, relative };
}

function textResult(text) {
  return { content: [{ type: "text", text }] };
}

function errorResult(action, error) {
  return {
    isError: true,
    content: [
      {
        type: "text",
        text: `${action}: ${error instanceof Error ? error.message : String(error)}`,
      },
    ],
  };
}

async function pathExists(target) {
  try {
    await fs.access(target);
    return true;
  } catch {
    return false;
  }
}

function createMcpServer() {
  const server = new McpServer({
    name: "my-pc-filesystem",
    version: "1.1.0",
  });

  server.tool(
    "list_files",
    "Показать файлы и папки внутри разрешённой директории",
    {
      relativePath: z.string().describe('Путь к папке внутри разрешённой директории. Для корня укажи "."'),
    },
    async ({ relativePath }) => {
      try {
        const { target } = safePath(relativePath);
        const entries = await fs.readdir(target, { withFileTypes: true });
        const formatted = entries
          .map((entry) => `${entry.isDirectory() ? "📁" : "📄"} ${entry.name}`)
          .sort((a, b) => a.localeCompare(b, "ru"))
          .join("\n");
        return textResult(formatted || `Папка пуста: ${relativePath}`);
      } catch (error) {
        return errorResult("Ошибка чтения папки", error);
      }
    },
  );

  server.tool(
    "read_text_file",
    "Прочитать текстовый файл из разрешённой директории",
    {
      relativePath: z.string().describe("Относительный путь к файлу"),
    },
    async ({ relativePath }) => {
      try {
        const { target } = safePath(relativePath);
        return textResult(await fs.readFile(target, "utf8"));
      } catch (error) {
        return errorResult("Ошибка чтения файла", error);
      }
    },
  );

  server.tool(
    "write_text_file",
    "Создать или полностью перезаписать текстовый файл",
    {
      relativePath: z.string().describe("Относительный путь к файлу"),
      content: z.string().describe("Новое содержимое файла"),
      overwrite: z.boolean().describe("true — разрешить перезапись существующего файла"),
    },
    async ({ relativePath, content, overwrite }) => {
      try {
        const { target } = safePath(relativePath);
        await fs.mkdir(path.dirname(target), { recursive: true });
        await fs.writeFile(target, content, { encoding: "utf8", flag: overwrite ? "w" : "wx" });
        return textResult(`Файл записан: ${relativePath}`);
      } catch (error) {
        return errorResult("Ошибка записи файла", error);
      }
    },
  );

  server.tool(
    "append_text_file",
    "Добавить текст в конец файла",
    {
      relativePath: z.string().describe("Относительный путь к файлу"),
      content: z.string().describe("Текст для добавления"),
    },
    async ({ relativePath, content }) => {
      try {
        const { target } = safePath(relativePath);
        await fs.mkdir(path.dirname(target), { recursive: true });
        await fs.appendFile(target, content, "utf8");
        return textResult(`Текст добавлен: ${relativePath}`);
      } catch (error) {
        return errorResult("Ошибка добавления текста", error);
      }
    },
  );

  server.tool(
    "edit_text_file",
    "Заменить точный фрагмент текста в файле",
    {
      relativePath: z.string().describe("Относительный путь к файлу"),
      oldText: z.string().min(1).describe("Точный текст, который нужно найти"),
      newText: z.string().describe("Текст для замены"),
      replaceAll: z.boolean().describe("true — заменить все совпадения, false — только первое"),
    },
    async ({ relativePath, oldText, newText, replaceAll }) => {
      try {
        const { target } = safePath(relativePath);
        const original = await fs.readFile(target, "utf8");
        if (!original.includes(oldText)) {
          throw new Error("Указанный фрагмент не найден.");
        }
        const updated = replaceAll ? original.split(oldText).join(newText) : original.replace(oldText, newText);
        await fs.writeFile(target, updated, "utf8");
        return textResult(`Файл изменён: ${relativePath}`);
      } catch (error) {
        return errorResult("Ошибка изменения файла", error);
      }
    },
  );

  server.tool(
    "create_directory",
    "Создать папку внутри разрешённой директории",
    {
      relativePath: z.string().describe("Относительный путь к новой папке"),
    },
    async ({ relativePath }) => {
      try {
        const { target } = safePath(relativePath);
        await fs.mkdir(target, { recursive: true });
        return textResult(`Папка создана: ${relativePath}`);
      } catch (error) {
        return errorResult("Ошибка создания папки", error);
      }
    },
  );

  server.tool(
    "move_path",
    "Переместить или переименовать файл либо папку",
    {
      sourcePath: z.string().describe("Исходный относительный путь"),
      destinationPath: z.string().describe("Новый относительный путь"),
      overwrite: z.boolean().describe("true — заменить существующий путь назначения"),
    },
    async ({ sourcePath, destinationPath, overwrite }) => {
      try {
        const source = safePath(sourcePath).target;
        const destination = safePath(destinationPath).target;
        await fs.mkdir(path.dirname(destination), { recursive: true });
        if (await pathExists(destination)) {
          if (!overwrite) throw new Error("Путь назначения уже существует.");
          await fs.rm(destination, { recursive: true, force: true });
        }
        await fs.rename(source, destination);
        return textResult(`Перемещено: ${sourcePath} → ${destinationPath}`);
      } catch (error) {
        return errorResult("Ошибка перемещения", error);
      }
    },
  );

  server.tool(
    "copy_path",
    "Скопировать файл или папку",
    {
      sourcePath: z.string().describe("Исходный относительный путь"),
      destinationPath: z.string().describe("Относительный путь копии"),
      overwrite: z.boolean().describe("true — разрешить замену существующих файлов"),
    },
    async ({ sourcePath, destinationPath, overwrite }) => {
      try {
        const source = safePath(sourcePath).target;
        const destination = safePath(destinationPath).target;
        await fs.mkdir(path.dirname(destination), { recursive: true });
        await fs.cp(source, destination, { recursive: true, force: overwrite, errorOnExist: !overwrite });
        return textResult(`Скопировано: ${sourcePath} → ${destinationPath}`);
      } catch (error) {
        return errorResult("Ошибка копирования", error);
      }
    },
  );

  server.tool(
    "delete_path",
    "Удалить файл или папку. Требует явного подтверждения DELETE",
    {
      relativePath: z.string().describe("Относительный путь к файлу или папке"),
      recursive: z.boolean().describe("true — разрешить удаление непустой папки"),
      confirmation: z.literal("DELETE").describe('Для подтверждения передай строку "DELETE"'),
    },
    async ({ relativePath, recursive }) => {
      try {
        const { root, target } = safePath(relativePath);
        if (target === root) throw new Error("Удалять корневую разрешённую папку запрещено.");
        await fs.rm(target, { recursive, force: false });
        return textResult(`Удалено: ${relativePath}`);
      } catch (error) {
        return errorResult("Ошибка удаления", error);
      }
    },
  );

  server.tool(
    "get_file_info",
    "Показать размер, тип и время изменения файла или папки",
    {
      relativePath: z.string().describe("Относительный путь"),
    },
    async ({ relativePath }) => {
      try {
        const { target } = safePath(relativePath);
        const stat = await fs.stat(target);
        return textResult(JSON.stringify({
          path: relativePath,
          type: stat.isDirectory() ? "directory" : "file",
          sizeBytes: stat.size,
          modifiedAt: stat.mtime.toISOString(),
        }, null, 2));
      } catch (error) {
        return errorResult("Ошибка получения информации", error);
      }
    },
  );

  return server;
}

/*
 * Современный Streamable HTTP:
 * https://домен/mcp
 */
app.post("/mcp", async (req, res) => {
  const server = createMcpServer();

  const transport = new StreamableHTTPServerTransport({
    sessionIdGenerator: undefined,
  });

  try {
    await server.connect(transport);
    await transport.handleRequest(req, res, req.body);
  } catch (error) {
    console.error("[MCP] Streamable HTTP error:", error);

    if (!res.headersSent) {
      res.status(500).json({
        jsonrpc: "2.0",
        id: req.body?.id ?? null,
        error: {
          code: -32603,
          message: "Internal MCP server error",
        },
      });
    }
  } finally {
    await transport.close().catch(() => {});
    await server.close().catch(() => {});
  }
});

app.get("/mcp", (_req, res) => {
  res.status(405).set("Allow", "POST").send("Use POST /mcp");
});

/*
 * Legacy SSE:
 * https://домен/sse
 */
const sseSessions = new Map();

app.get("/sse", async (_req, res) => {
  const server = createMcpServer();
  const transport = new SSEServerTransport("/messages", res);

  sseSessions.set(transport.sessionId, {
    server,
    transport,
  });

  console.log(`[MCP] SSE connected: ${transport.sessionId}`);

  res.on("close", async () => {
    sseSessions.delete(transport.sessionId);

    await transport.close().catch(() => {});
    await server.close().catch(() => {});

    console.log(`[MCP] SSE disconnected: ${transport.sessionId}`);
  });

  try {
    await server.connect(transport);
  } catch (error) {
    console.error("[MCP] SSE connection error:", error);
    sseSessions.delete(transport.sessionId);

    if (!res.headersSent) {
      res.status(500).end();
    }
  }
});

app.post("/messages", async (req, res) => {
  const sessionId = String(req.query.sessionId ?? "");
  const session = sseSessions.get(sessionId);

  if (!session) {
    return res.status(404).json({
      error: "Unknown or expired SSE session",
    });
  }

  try {
    await session.transport.handlePostMessage(req, res, req.body);
  } catch (error) {
    console.error("[MCP] SSE message error:", error);

    if (!res.headersSent) {
      res.status(500).json({
        error: "Failed to process MCP message",
      });
    }
  }
});

app.get("/", (_req, res) => {
  res.json({
    name: "my-pc-filesystem",
    status: "ok",
    directory: ALLOWED_DIRECTORY,
    endpoints: {
      streamableHttp: "/mcp",
      sse: "/sse",
    },
    authentication: MCP_TOKEN ? "Bearer token" : "disabled",
  });
});

app.listen(PORT, "0.0.0.0", () => {
  console.log(`[MCP] Сервер запущен: http://127.0.0.1:${PORT}`);
  console.log(`[MCP] Разрешённая папка: ${ALLOWED_DIRECTORY}`);
  console.log(`[MCP] Streamable HTTP: http://127.0.0.1:${PORT}/mcp`);
  console.log(`[MCP] SSE: http://127.0.0.1:${PORT}/sse`);

  if (!MCP_TOKEN) {
    console.warn("[MCP] ВНИМАНИЕ: авторизация отключена!");
  }
});