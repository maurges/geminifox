// The manifest passes the location after a sha-bang, it's url-encoded
const loc = decodeURIComponent(window.location.hash.slice(2)).slice(4);
// Todo: validate the location before passing it to native

document.write("<p>will navigate to " + loc + "</p>");

function displayBody(bytes) {
    const dec = new TextDecoder();
}

function displayCode(b1, b2) {
    return String.fromCharCode(b1) + String.fromCharCode(b2);
}

function displayHeader(bytes) {
    const dec = new TextDecoder();

    let code = displayCode(bytes[0], bytes[1]);

    // 1X - Input required
    if (bytes[0] === 49) {
        // 11 - Sensitive input
        if (bytes[1] === 49) {
            document.write(`<p>${code} Sensitive Input Required</p>`);
        } else {
            document.write(`<p>${code} Input Required</p>`);
        }
        const question = dec.decode(bytes.slice(3));
        document.write("<p>" + question + "</p>");

        return false;

    // 2X - Success
    } else if (bytes[0] == 50) {
        document.write(`<p>${code} Success</p>`);

        return true;

    // 3X - Redirect
    } else if (bytes[0] == 51) {
        // 30 - Temporary
        if (bytes[1] == 48) {
            document.write(`<p>${code} Temporary Redirect</p>`);
        // 31 - Permanent
        } else if (bytes[1] === 49) {
            document.write(`<p>${code} Permanent Redirect</p>`);
        } else {
            document.write(`<p>${code} Redirect</p>`);
        }

        const target = dec.decode(bytes.slice(3));
        document.write(`<a href="web+${target}"> ${target} </a>`);

        return false;

    // 4X - Temporary failure
    } else if (bytes[0] === 52) {
        // 41 - unavailable
        if (bytes[1] === 49) {
            document.write(`<p>${code} Server Unavailable</p>`);
        // 42 - cgi
        } else if (bytes[1] === 50) {
            document.write(`<p>${code} CGI Error </p>`);
        // 43 - proxy
        } else if (bytes[1] === 51) {
            document.write(`<p>${code} Proxy Error </p>`);
        // 44 - slow down
        } else if (bytes[1] === 52) {
            document.write(`<p>${code} Too Many Requests </p>`);
        } else {
            document.write(`<p>${code} Temporary Server Failure </p>`);
        }
        const message = dec.decode(bytes.slice(3));
        document.write("<p>" + message + "</p>");

        return false;

    // 5X - Permanent failure
    } else if (bytes[0] === 53) {
        // 51 - not found
        if (bytes[1] === 49) {
            document.write(`<p>${code} Not Found </p>`);
        // 52 - gone
        } else if (bytes[1] === 50) {
            document.write(`<p>${code} Resource No Longer Available </p>`);
        // 53 - proxy
        } else if (bytes[1] === 51) {
            document.write(`<p>${code} Proxy Request Refused </p>`);
        // 59 - bad request
        } else if (bytes[1] === 57) {
            document.write(`<p>${code} Bad Request </p>`);
        } else {
            document.write(`<p>${code} Permanent Server Failure </p>`);
        }
        const message = dec.decode(bytes.slice(3));
        document.write("<p>" + message + "</p>");

        return false;
    }
}

async function run(loc) {
    function responseReceived(responseB64) {
        const resp = Uint8Array.fromBase64(responseB64);

        // Find the end of the header by "\r\n"
        let i = 0;
        while (resp[i] != 13 && resp[i+1] != 10) {
            i += 1;
        }
        const header = resp.slice(0, i);
        const body = resp.slice(i+4);

        const hasBody = displayHeader(header);
        if (hasBody) {
            const bodyText = (new TextDecoder()).decode(body);
            console.log("Body text", bodyText);
            document.write(bodyText);
        }
    }

    const port = browser.runtime.connect();
    port.onMessage.addListener(responseReceived);

    const messageArray = (new TextEncoder()).encode(loc);
    const messageB64 = messageArray.toBase64();
    port.postMessage(messageB64);
}

run(loc)
