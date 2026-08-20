#!/usr/bin/env node

import { createHash, randomUUID } from "node:crypto";
import { readFile } from "node:fs/promises";

function parseArguments(values)
{
    const options = {
        config: "",
        request: "",
        data: {},
        timeoutMilliseconds: 10_000,
    };
    for (let index = 0; index < values.length; ++index)
    {
        const value = values[index];
        if (value === "--config")
        {
            options.config = values[++index] ?? "";
        }
        else if (value === "--request")
        {
            options.request = values[++index] ?? "";
        }
        else if (value === "--data")
        {
            options.data = JSON.parse(values[++index] ?? "{}");
        }
        else if (value === "--data-base64")
        {
            const encoded = values[++index] ?? "e30=";
            options.data = JSON.parse(Buffer.from(encoded, "base64").toString("utf8"));
        }
        else if (value === "--timeout-ms")
        {
            options.timeoutMilliseconds = Number(values[++index]);
        }
        else
        {
            throw new Error(`unknown argument: ${value}`);
        }
    }
    if (!options.config || !options.request)
    {
        throw new Error("--config and --request are required");
    }
    if (!Number.isInteger(options.timeoutMilliseconds)
        || options.timeoutMilliseconds <= 0
        || options.timeoutMilliseconds > 60_000)
    {
        throw new Error("--timeout-ms must be between 1 and 60000");
    }
    return options;
}

function base64Sha256(value)
{
    return createHash("sha256").update(value, "utf8").digest("base64");
}

function authenticationValue(password, salt, challenge)
{
    const secret = base64Sha256(password + salt);
    return base64Sha256(secret + challenge);
}

async function sendRequest(options)
{
    const config = JSON.parse(await readFile(options.config, "utf8"));
    const port = Number(config.server_port);
    if (!Number.isInteger(port) || port <= 0 || port > 65_535)
    {
        throw new Error("OBS WebSocket config contains an invalid port");
    }
    const requestId = randomUUID();

    return await new Promise((resolve, reject) =>
    {
        const socket = new WebSocket(`ws://127.0.0.1:${port}`);
        let requestSent = false;
        let settled = false;
        const finish = (action, value) =>
        {
            if (settled)
            {
                return;
            }
            settled = true;
            clearTimeout(timer);
            socket.close();
            action(value);
        };
        const timer = setTimeout(
            () => finish(reject, new Error("OBS WebSocket request timed out")),
            options.timeoutMilliseconds);

        socket.addEventListener("error", () =>
        {
            finish(reject, new Error("could not connect to OBS WebSocket"));
        });
        socket.addEventListener("message", (event) =>
        {
            let message;
            try
            {
                message = JSON.parse(String(event.data));
            }
            catch (error)
            {
                finish(reject, new Error("OBS WebSocket returned invalid JSON", { cause: error }));
                return;
            }

            if (message.op === 0)
            {
                const identify = { rpcVersion: 1 };
                const authentication = message.d?.authentication;
                if (authentication != null)
                {
                    if (typeof config.server_password !== "string")
                    {
                        finish(reject, new Error("OBS WebSocket password is unavailable"));
                        return;
                    }
                    identify.authentication = authenticationValue(
                        config.server_password,
                        authentication.salt,
                        authentication.challenge);
                }
                socket.send(JSON.stringify({ op: 1, d: identify }));
                return;
            }
            if (message.op === 2 && !requestSent)
            {
                requestSent = true;
                socket.send(JSON.stringify({
                    op: 6,
                    d: {
                        requestType: options.request,
                        requestId,
                        requestData: options.data,
                    },
                }));
                return;
            }
            if (message.op !== 7 || message.d?.requestId !== requestId)
            {
                return;
            }
            if (message.d.requestStatus?.result !== true)
            {
                const code = message.d.requestStatus?.code ?? "unknown";
                const comment = message.d.requestStatus?.comment ?? "request failed";
                finish(reject, new Error(`OBS request failed (${code}): ${comment}`));
                return;
            }
            finish(resolve, message.d.responseData ?? {});
        });
    });
}

try
{
    const options = parseArguments(process.argv.slice(2));
    const response = await sendRequest(options);
    process.stdout.write(`${JSON.stringify(response)}\n`);
}
catch (error)
{
    process.stderr.write(`OBS WebSocket request failed: ${error.message}\n`);
    process.exitCode = 1;
}
