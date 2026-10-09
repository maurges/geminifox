/// <reference path="../types/webext.d.ts" />

//! Simply resends messages to the native program, since you can't bloody do it
//from a content script

/**
 * @param {object} message
 * @param {browser.runtime.Port} pagePort
 */
function messageToNative(message, pagePort) {
    const natport = browser.runtime.connectNative("geminifox");

    let didRespond = false;
    natport.onDisconnect.addListener(p => {
        if (!didRespond) {
            console.log("Disconnected with error", p.error);
            pagePort.postMessage({"error": "NATIVE_FAILED"})
        }
        pagePort.disconnect();
    });
    if (natport.error) {
        console.log("Error connecting", natport.error);
        pagePort.postMessage({"error": "NATIVE_FAILED"})
        pagePort.disconnect();
    };

    natport.onMessage.addListener(msg => {
        didRespond = true;
        return pagePort.postMessage(msg);
    });
    natport.postMessage(message);
}

/**
 * @param {browser.runtime.Port} port
 */
function proxyConnected(port) {
    port.onMessage.addListener(message => messageToNative(
        message,
        port,
    ));
}

browser.runtime.onConnect.addListener(proxyConnected);
