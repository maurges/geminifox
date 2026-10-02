/// <reference path="../types/webext.d.ts" />

//! Simply resends messages to the native program, since you can't bloody do it
//from a content script

/**
 * @param {object} message
 * @param {(msg: unknown) => void} onMessage
 */
function messageToNative(message, onMessage) {
    const natport = browser.runtime.connectNative("gemini_browser");

    let didRespond = false;
    natport.onDisconnect.addListener(p => {
        if (!didRespond) {
            console.log("Disconnected with error", p);
            onMessage({"error": "NATIVE_FAILED"})
        }
    });
    if (natport.error) {
        console.log("Error connecting", natport.error);
        onMessage({"error": "NATIVE_FAILED"})
    };

    natport.onMessage.addListener(msg => {
        didRespond = true;
        return onMessage(msg);
    });
    natport.postMessage(message);
}

/**
 * @param {browser.runtime.Port} port
 */
function proxyConnected(port) {
    port.onMessage.addListener(message => messageToNative(
        message,
        msg => port.postMessage(msg),
    ));
}

browser.runtime.onConnect.addListener(proxyConnected);
