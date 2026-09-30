/// <reference path="../types/webext.d.ts" />


/****************************/
/****** Error handling ******/
/****************************/


/**
 * @param {string} s
 * @returns {never}
 */
function panic(s) {
    throw new Error("Panic: " + s);
}

/**
 * @template A
 * @param {A | null | undefined} x
 * @param {string} s - error message
 * @returns {A}
 */
function expect(x, s) {
    if (x === null || x === undefined) {
        panic("unwrap: " + s);
    }
    return x;
}


/****************************/
/****** Main extension ******/
/****************************/


function messageToNative(messageB64) {
    // Todo: I want support for status messages, like connecting to network, dns failure, etc
    const natport = browser.runtime.connectNative("gemini_browser");
    natport.onDisconnect.addListener(p => {
        console.log("Disconnected with error", p);
    });
    if (natport.error) {
        console.log("Error connecting", natport.error);
    };
    const r = new Promise((resolve, reject) => {
        natport.onMessage.addListener(msg => {
            natport.disconnect();
            resolve(msg);
        })
    });
    natport.postMessage(messageB64);
    return r;
}

function proxyConnected(port) {
    async function onMessage(messageB64) {

        const resp = await messageToNative(messageB64);
        port.postMessage(resp);
    }
    port.onMessage.addListener(onMessage);
}

browser.runtime.onConnect.addListener(proxyConnected);
