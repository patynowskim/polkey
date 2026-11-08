// Transport: WebUSB tunnel to the PolKey (BL616). The JSON messages of the old serial protocol travel in frames
// [type][len lo][len hi][payload] over the vendor bulk interface (OUT endpoint 1, IN endpoint 2).
const POLKEY_VID = 0x1209, POLKEY_PID = 0xC0CA;
const TUNNEL_INTERFACE = 0, EP_OUT = 1, EP_IN = 2, TUNNEL_CMD = 0xF0, TUNNEL_CMD_MORE = 0xF1;
let usbDev = null;
let requestChain = Promise.resolve();
let isConnected = false;
let authState = "UNKNOWN";
let pendingAutoGenerate = null;
let pendingGetPassword = null;

const terminal = document.getElementById('terminal');
const connectBtn = document.getElementById('connectBtn');
const inputField = document.getElementById('input');
const sendBtn = document.getElementById('sendBtn');

function setAuthState(newState) {
    if (authState !== newState) {
        authState = newState;
        chrome.runtime.sendMessage({
            target: "background",
            type: "STATE_CHANGED",
            authState: authState
        }).catch(() => {});
    }
}

function bufferToHex(buffer) {
    return Array.from(new Uint8Array(buffer)).map(b => b.toString(16).padStart(2, '0')).join('');
}

function hexToBuffer(hex) {
    const cleanHex = hex.replace(/[^0-9a-fA-F]/g, '');
    const bytes = new Uint8Array(Math.ceil(cleanHex.length / 2));
    for (let i = 0; i < bytes.length; i++) {
        bytes[i] = parseInt(cleanHex.substring(i * 2, i * 2 + 2), 16);
    }
    return bytes;
}

window.addEventListener('DOMContentLoaded', async () => {
    const devs = await navigator.usb.getDevices();
    if (devs.some(d => d.vendorId === POLKEY_VID && d.productId === POLKEY_PID)) await connect();

    navigator.usb.addEventListener('connect', async () => { if (!isConnected) await connect(); });
    navigator.usb.addEventListener('disconnect', async (e) => {
        if (isConnected && usbDev && e.device === usbDev) await disconnect();
    });
});

connectBtn.onclick = async () => {
    if (isConnected) await disconnect();
    else await connect();
};

async function connect() {
    try {
        const devs = await navigator.usb.getDevices();
        usbDev = devs.find(d => d.vendorId === POLKEY_VID && d.productId === POLKEY_PID) ||
                 await navigator.usb.requestDevice({ filters: [{ vendorId: POLKEY_VID, productId: POLKEY_PID }] });

        await usbDev.open();
        if (usbDev.configuration === null) await usbDev.selectConfiguration(1);
        await usbDev.claimInterface(TUNNEL_INTERFACE);

        connectBtn.innerText = "Disconnect";
        inputField.disabled = false;
        sendBtn.disabled = false;
        isConnected = true;

        terminal.innerText += "[System] Connected to PolKey.\n";
        await sendSecure({ cmd: "RESTART_SYSTEM" });
    } catch (err) {
        terminal.innerText += `[System Error] Initialization failed: ${err && err.message ? err.message : err}\n`;
        isConnected = false;
    }
}

async function disconnect() {
    if (usbDev && isConnected) {
        try {
            await sendSecure({ cmd: "DISCONNECT" });
            await new Promise(resolve => setTimeout(resolve, 100));
        } catch (err) {}
    }
    isConnected = false;
    setAuthState("UNKNOWN");
    pendingAutoGenerate = null;
    pendingGetPassword = null;

    if (usbDev) {
        try { await usbDev.releaseInterface(TUNNEL_INTERFACE); } catch (e) {}
        try { await usbDev.close(); } catch (e) {}
        usbDev = null;
    }
    window.close();
}

function processIncomingLine(text) {
    let jsonMsg = null;
    try {
        jsonMsg = JSON.parse(text);
    } catch (e) {
        // Fallback for non-JSON or raw system lines
    }

    if (jsonMsg) {
        if (jsonMsg.type === "event") {
            if (jsonMsg.module === "SYS" && jsonMsg.event === "BOOT") {
                setAuthState(jsonMsg.data && jsonMsg.data.pin_set ? "PIN_REQ" : "NEW_PIN_REQ");
            } else if (jsonMsg.module === "AUTH" && jsonMsg.event === "PIN_CREATED") {
                setAuthState("PIN_REQ");
            } else if (jsonMsg.module === "SECURITY" && jsonMsg.event === "PIN_OK") {
                setAuthState("READY");
            } else if (jsonMsg.module === "PASS" && jsonMsg.event === "AWAITING_HARDWARE_APPROVAL") {
                setAuthState("AWAITING_FINGERPRINT");
            } else if (jsonMsg.module === "PASS" && jsonMsg.event === "TRANSMITTED") {
                setAuthState("READY");
                const passwordValue = jsonMsg.data ? jsonMsg.data.password : null;
                if (passwordValue && pendingGetPassword) {
                    const targetObj = pendingGetPassword;
                    pendingGetPassword = null;
                    fillCredentialsInTab(targetObj, passwordValue);
                }
            } else if (jsonMsg.module === "PASS" && jsonMsg.event === "SAVED") {
                setAuthState("READY");
                const passwordValue = jsonMsg.data ? jsonMsg.data.generated_password : null;
                if (passwordValue && pendingAutoGenerate) {
                    const targetObj = pendingAutoGenerate;
                    pendingAutoGenerate = null;
                    fillCredentialsInTab(targetObj, passwordValue);
                }
            } else if (jsonMsg.module === "PASS" && jsonMsg.event === "UPDATED") {
                setAuthState("READY");
            } else if (jsonMsg.module === "BACKUP") {
                if (jsonMsg.event === "EXPORT_START" && jsonMsg.data) {
                    backupExportBuffer = {
                        chunks: new Array(jsonMsg.data.total_chunks).fill(""),
                        totalChunks: jsonMsg.data.total_chunks,
                        totalLen: jsonMsg.data.total_len,
                        sha256: jsonMsg.data.sha256,
                        received: 0
                    };
                    notifyBackupProgress("export", 5, "Receiving encrypted archive...");
                } else if (jsonMsg.event === "EXPORT_CHUNK" && jsonMsg.data && backupExportBuffer) {
                    backupExportBuffer.chunks[jsonMsg.data.seq] = jsonMsg.data.data;
                    backupExportBuffer.received++;
                    const pct = 5 + Math.round((backupExportBuffer.received / backupExportBuffer.totalChunks) * 90);
                    notifyBackupProgress("export", pct, "Receiving encrypted archive...");
                } else {
                    dispatchBackupEvent(jsonMsg);
                }
            }
        } else if (jsonMsg.type === "error") {
            if (jsonMsg.error_code === "PIN_REQ") setAuthState("PIN_REQ");
            else if (jsonMsg.error_code === "NEW_PIN_REQ") setAuthState("NEW_PIN_REQ");
            else if (jsonMsg.error_code === "BAD_PIN_ATTEMPT") terminal.innerText += "Wrong PIN\n";
            else if (jsonMsg.module === "BACKUP") {
                dispatchBackupEvent(jsonMsg);
            }
            else if (jsonMsg.error_code === "NOT_FOUND") {
                if (pendingGetPassword) {
                    terminal.innerText += `[System] Password not found.\n`;
                    pendingGetPassword = null;
                    setAuthState("READY");
                }
            }
        }
    }

    let safeText = text;
    if (jsonMsg && jsonMsg.type === "event" && jsonMsg.event === "TRANSMITTED" && jsonMsg.data && jsonMsg.data.password) {
        const maskedMsg = JSON.parse(JSON.stringify(jsonMsg));
        maskedMsg.data.password = "********";
        safeText = JSON.stringify(maskedMsg);
    } else if (jsonMsg && jsonMsg.type === "event" && jsonMsg.event === "SAVED" && jsonMsg.data && jsonMsg.data.generated_password) {
        const maskedMsg = JSON.parse(JSON.stringify(jsonMsg));
        maskedMsg.data.generated_password = "********";
        safeText = JSON.stringify(maskedMsg);
    }

    terminal.innerText += safeText + "\n";
    terminal.scrollTop = terminal.scrollHeight;

    chrome.runtime.sendMessage({ target: "popup", type: "SERIAL_OUTPUT", text: safeText, json: jsonMsg }).catch(() => {});
}

let backupBusy = false;
let backupExportBuffer = null;
let backupEventListeners = [];

function waitForBackupEvent(matchFn, timeoutMs = 30000) {
    return new Promise((resolve, reject) => {
        const timer = setTimeout(() => {
            backupEventListeners = backupEventListeners.filter(l => l !== listener);
            reject(new Error("Timed out waiting for the device."));
        }, timeoutMs);
        const listener = (msg) => {
            if (matchFn(msg)) {
                clearTimeout(timer);
                backupEventListeners = backupEventListeners.filter(l => l !== listener);
                resolve(msg);
            }
        };
        backupEventListeners.push(listener);
    });
}

function dispatchBackupEvent(msg) {
    backupEventListeners.slice().forEach(l => l(msg));
}

function notifyBackupProgress(phase, percent, message) {
    chrome.runtime.sendMessage({ target: "popup", type: "BACKUP_PROGRESS", phase, percent, message }).catch(() => {});
}

function notifyBackupResult(phase, success, message) {
    chrome.runtime.sendMessage({ target: "popup", type: "BACKUP_RESULT", phase, success, message }).catch(() => {});
}

async function runBackupExport(passphrase) {
    if (backupBusy) { notifyBackupResult("export", false, "Another backup operation is already running."); return; }
    backupBusy = true;
    backupExportBuffer = null;

    try {
        notifyBackupProgress("export", 0, "Requesting export...");
        await sendSecure({ cmd: "BACKUP_EXPORT", passphrase: passphrase });

        const doneMsg = await waitForBackupEvent((m) =>
            (m.type === "event" && m.module === "BACKUP" && m.event === "EXPORT_DONE") ||
            (m.type === "error" && m.module === "BACKUP"),
        60000);
        if (doneMsg.type === "error") throw new Error(doneMsg.message || doneMsg.error_code);

        const buf = backupExportBuffer;
        if (!buf) throw new Error("No archive data received.");

        const hexBlob = buf.chunks.join("");
        if (hexBlob.length !== buf.totalLen) throw new Error("Incomplete transfer from device.");

        const digestBuf = await crypto.subtle.digest("SHA-256", new TextEncoder().encode(hexBlob));
        const actualSha256 = bufferToHex(digestBuf);
        if (actualSha256 !== buf.sha256) throw new Error("Checksum mismatch -- transfer may have been corrupted, try again.");

        const blob = new Blob([hexBlob], { type: "text/plain" });
        const url = URL.createObjectURL(blob);
        const stamp = new Date().toISOString().replace(/[:.]/g, "-");

        await chrome.downloads.download({
            url: url,
            filename: `vault-backup-${stamp}.vbk`,
            saveAs: true
        });
        setTimeout(() => URL.revokeObjectURL(url), 60000);

        notifyBackupResult("export", true, "Backup saved.");
    } catch (err) {
        notifyBackupResult("export", false, err.message || String(err));
    } finally {
        backupExportBuffer = null;
        backupBusy = false;
    }
}

async function runBackupImport(hexBlob, sha256, passphrase, overwrite) {
    if (backupBusy) { notifyBackupResult("import", false, "Another backup operation is already running."); return; }
    backupBusy = true;

    try {
        notifyBackupProgress("import", 0, "Starting restore...");
        await sendSecure({ cmd: "BACKUP_IMPORT_BEGIN", total_len: hexBlob.length, sha256: sha256 });

        const readyMsg = await waitForBackupEvent((m) =>
            (m.type === "event" && m.module === "BACKUP" && m.event === "IMPORT_READY") ||
            (m.type === "error" && m.module === "BACKUP"),
        15000);
        if (readyMsg.type === "error") throw new Error(readyMsg.message || readyMsg.error_code);

        const CHUNK = 2048;
        const totalChunks = Math.max(1, Math.ceil(hexBlob.length / CHUNK));
        for (let i = 0; i < totalChunks; i++) {
            const slice = hexBlob.substring(i * CHUNK, (i + 1) * CHUNK);
            await sendSecure({ cmd: "BACKUP_IMPORT_CHUNK", data: slice });
            notifyBackupProgress("import", 5 + Math.round(((i + 1) / totalChunks) * 70), "Uploading archive...");
        }

        notifyBackupProgress("import", 80, "Decrypting and restoring...");
        await sendSecure({ cmd: "BACKUP_IMPORT_COMMIT", passphrase: passphrase, overwrite: !!overwrite });

        const doneMsg = await waitForBackupEvent((m) =>
            (m.type === "event" && m.module === "BACKUP" && m.event === "IMPORT_DONE") ||
            (m.type === "error" && m.module === "BACKUP"),
        30000);
        if (doneMsg.type === "error") throw new Error(doneMsg.message || doneMsg.error_code);

        const data = doneMsg.data || {};
        notifyBackupResult("import", true, `Restored ${data.imported || 0} item(s), skipped ${data.skipped || 0}.`);
    } catch (err) {
        try { await sendSecure({ cmd: "BACKUP_IMPORT_ABORT" }); } catch (e) {}
        notifyBackupResult("import", false, err.message || String(err));
    } finally {
        backupBusy = false;
    }
}

function fillCredentialsInTab(targetObj, passwordValue) {
    const targetTabId = targetObj ? targetObj.tabId : null;
    const targetLogin = targetObj ? targetObj.login : null;

    if (targetTabId) {
        chrome.tabs.sendMessage(targetTabId, { type: "FILL_CREDENTIALS", password: passwordValue, login: targetLogin }).catch(() => {});
    } else {
        chrome.tabs.query({ active: true, currentWindow: true }, (tabs) => {
            if (tabs.length > 0) chrome.tabs.sendMessage(tabs[0].id, { type: "FILL_CREDENTIALS", password: passwordValue, login: targetLogin }).catch(() => {});
        });
    }
}

// One request at a time. The answer is a sequence of frames: 0xF1 = a JSON message (more may follow), 0xF0 with an
// empty payload = answer complete. Messages are handed to the UI logic as they arrive (backup export streams).
async function tunnelRequest(text, onMessage) {
    const payload = new TextEncoder().encode(text);
    const frame = new Uint8Array(3 + payload.length);
    frame[0] = TUNNEL_CMD;
    frame[1] = payload.length & 0xFF;
    frame[2] = payload.length >> 8;
    frame.set(payload, 3);
    for (let off = 0; off < frame.length; off += 64) {
        await usbDev.transferOut(EP_OUT, frame.slice(off, off + 64));
    }

    let pending = new Uint8Array(0);
    let deadline = Date.now() + 40000; // PBKDF2 (backup: 200000 iterations) can take several seconds
    for (;;) {
        // complete frames already buffered?
        while (pending.length >= 3) {
            const type = pending[0], len = pending[1] | (pending[2] << 8);
            if (pending.length < 3 + len) break;
            const body = pending.slice(3, 3 + len);
            pending = pending.slice(3 + len);
            if (type === TUNNEL_CMD) return; // end of answer
            if (type === TUNNEL_CMD_MORE) {
                onMessage(new TextDecoder().decode(body));
                deadline = Date.now() + 40000;
            }
        }
        if (Date.now() > deadline) throw new Error("timeout waiting for the key");
        const res = await usbDev.transferIn(EP_IN, 64);
        if (res.status !== "ok" || !res.data || res.data.byteLength === 0) continue;
        const chunk = new Uint8Array(res.data.buffer, res.data.byteOffset, res.data.byteLength);
        const merged = new Uint8Array(pending.length + chunk.length);
        merged.set(pending);
        merged.set(chunk, pending.length);
        pending = merged;
    }
}

function sendSecure(payload) {
    const text = (typeof payload === "object") ? JSON.stringify(payload) : payload;
    if (!usbDev || !isConnected) return Promise.resolve();
    requestChain = requestChain.then(async () => {
        try {
            await tunnelRequest(text, processIncomingLine);
        } catch (e) {
            terminal.innerText += `[System Error] ${e && e.message ? e.message : e}\n`;
        }
    });
    return requestChain;
}

const handleSendAction = async () => {
    if (inputField.value.trim() === "") return;
    let payload = inputField.value.trim();
    if (payload.startsWith("{") && payload.endsWith("}")) {
        try { payload = JSON.parse(payload); } catch (e) {}
    }
    await sendSecure(payload); 
    inputField.value = "";
};

document.getElementById('sendBtn').onclick = handleSendAction;
inputField.addEventListener("keyup", async (event) => { if (event.key === "Enter") await handleSendAction(); });

chrome.runtime.onMessage.addListener((message, sender, sendResponse) => {
    if (message.type === "PING") {
        sendResponse({ connected: isConnected, authState: authState });
    } else if (message.type === "ACTIVE_SITE") {
        sendResponse({ status: "ok" });
    } else if (message.type === "AUTO_GENERATE") {
        if (isConnected && authState === "READY") {
            if (pendingAutoGenerate) return sendResponse({ status: "busy" });
            pendingAutoGenerate = { 
                domain: message.hostname, 
                login: message.login,
                tabId: message.senderTabId || (sender.tab && sender.tab.id)
            };
            sendSecure({ cmd: "SAVE_PASS", site: message.hostname, login: message.login, autogen: true });
        }
        sendResponse({ status: "ok" });
    } else if (message.type === "GET_PASSWORD") {
        if (isConnected && authState === "READY") {
            if (pendingGetPassword) return sendResponse({ status: "busy" });
            pendingGetPassword = {
                domain: message.hostname,
                login: message.login,
                tabId: message.senderTabId || (sender.tab && sender.tab.id)
            };
            sendSecure({ cmd: "GET_PASS", site: message.hostname, login: message.login });
        }
        sendResponse({ status: "ok" });
    } else if (message.type === "CMD_LIST_PASS") {
        sendSecure({ cmd: "LIST_PASS" });
        sendResponse({ status: "ok" });
    } else if (message.type === "CMD_LIST_FIDO") {
        sendSecure({ cmd: "LIST_FIDO" });
        sendResponse({ status: "ok" });
    } else if (message.type === "CMD_DELETE_PASS") {
        sendSecure({ cmd: "DELETE_PASS", site: message.name, login: message.login });
        sendResponse({ status: "ok" });
    } else if (message.type === "CMD_DELETE_FIDO") {
        sendSecure({ cmd: "DELETE_FIDO", site: message.domain });
        sendResponse({ status: "ok" });
    } else if (message.type === "CMD_UPDATE_SETTINGS") {
        const algId = (message.algId !== undefined) ? parseInt(message.algId, 10) : -7;
        sendSecure({ cmd: "UPDATE_SETTINGS", algId: algId });
        sendResponse({ status: "ok" });
    } else if (message.type === "SEND") {
        sendSecure(message.payload);
        sendResponse({ status: "ok" });
    } else if (message.type === "DISCONNECT") {
        disconnect();
        sendResponse({ status: "ok" });
    } else if (message.type === "CMD_GET_ALL_TOTP") {
        const epochNow = Math.floor(Date.now() / 1000);
        sendSecure({ cmd: "GET_TOTP", epoch: epochNow });
        sendResponse({ status: "ok" });
    } else if (message.type === "CMD_DELETE_TOTP") {
        sendSecure({ cmd: "DELETE_TOTP", name: message.name });
        sendResponse({ status: "ok" });
    } else if (message.type === "BACKUP_EXPORT") {
        runBackupExport(message.passphrase);
        sendResponse({ status: "ok" });
    } else if (message.type === "BACKUP_IMPORT") {
        runBackupImport(message.hex, message.sha256, message.passphrase, message.overwrite);
        sendResponse({ status: "ok" });
    }
    return true;
});