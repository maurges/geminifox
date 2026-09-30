// The manifest passes the location after a sha-bang, it's url-encoded
const loc = decodeURIComponent(window.location.hash.slice(2)).slice(4);

document.write("will navigate to " + loc);

function responseReceived(responseB64) {
    const responseArray = Uint8Array.fromBase64(responseB64);
    // or probably process it as bytes, we'll see
    const response = (new TextDecoder()).decode(responseArray);
    console.log("Response", response);
}

const port = browser.runtime.connect();
port.onMessage.addListener(responseReceived);

const messageArray = (new TextEncoder()).encode(loc);
const messageB64 = messageArray.toBase64();
port.postMessage(messageB64);
