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


// todo: listen for messages, and resend them to the native program

function messageToNative(messageB64) {
    const natport = browser.runtime.connectNative("gemini_browser");
    natport.onDisconnect.addListener(p => {
        console.log("Disconnected with error", p);
    });
    if (natport.error) {
        console.log("Error connecting", natport.error);
    };
    const r = new Promise((resolve, reject) => {
        natport.onMessage.addListener(msg => {
            console.log("response", msg);
            natport.disconnect();
            resolve(msg);
        })
    });
    natport.postMessage(messageB64);
    return r;
}

function proxyConnected(port) {
    async function onMessage(messageB64) {
        console.log("posting", messageB64);

        const resp = await messageToNative(messageB64);
        port.postMessage(resp);
    }
    port.onMessage.addListener(onMessage);
}

browser.runtime.onConnect.addListener(proxyConnected);
