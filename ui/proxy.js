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

/**
 * @template A
 * @param {A | null | undefined} x
 * @returns {A}
 */
function unwrap(x) {
    return expect(x, "unexpected value");
}


/*********************************/
/****** Main functionality ******/
/*********************************/


/** Adjust a href of a created element to be openable with this extension
 * @param {string} href
 * @param {string} currentLocation
 * @returns {string}
 */
function adjustHref(href, currentLocation) {
    const url = URL.parse(href, currentLocation);
    if (url !== null && url.protocol === "gemini:") {
        return "web+" + url.toString();
    } else {
        return href;
    }
}

/**
 * @param {Uint8Array} bytes
 * @param {string} currentLocation - used to replace hrefs to gemini
 * @returns HTMLElement
 */
function parseGemtext(bytes, currentLocation) {
    const dec = new TextDecoder();

    const article = document.createElement("article");

    let i = 0;

    function ws() {
        // Skip spaces and tabs
        while (bytes[i] == 32 || bytes[i] == 9) {
            i += 1;
        }
    }
    function tonl() {
        const r = bytes.indexOf(10, i);
        i = r === -1 ? bytes.length : r + 1;
        return r;
    }
    function textToNl() {
        const start = i;
        const end = tonl();
        return dec.decode(bytes.slice(start, end));
    }

    /** @type {null | HTMLElement} */
    let currentList = null;
    /** @type {null | HTMLElement} */
    let currentQuote = null;
    function finishBlocks() {
        if (currentList !== null) {
            article.appendChild(currentList);
            currentList = null;
        }
        if (currentQuote !== null) {
            article.appendChild(currentQuote);
            currentQuote = null;
        }
    }

    while (i < bytes.length) {
        // Start of a line

        // => - link
        if (bytes[i] == 61 && bytes[i+1] == 62) {
            finishBlocks();
            i += 2;
            ws();
            const hrefStart = i;
            // Url is delimited by whitespace
            while (bytes[i] != 32 && bytes[i] != 9 && bytes[i] != 10) {
                i += 1;
            }
            const hrefEnd = i;
            const href = dec.decode(bytes.slice(hrefStart, hrefEnd));
            ws();
            const mbText = textToNl();
            const text = mbText.length === 0 ? href : mbText;

            const container = document.createElement("p");
            const a = document.createElement("a");
            a.href = adjustHref(href, currentLocation);
            a.innerText = text;
            container.appendChild(a);
            article.appendChild(container);

        // # - heading
        } else if (bytes[i] === 35) {
            finishBlocks();
            let hashes = 0;
            while (bytes[i] === 35) {
                hashes += 1;
                i += 1;
            }
            hashes = Math.min(6, hashes);
            const h = document.createElement("h" + hashes.toString());

            ws();
            const text = textToNl();

            h.innerText = text;
            article.appendChild(h);

        // ``` - preformatted
        } else if (bytes[i] === 96 && bytes[i+1] === 96 && bytes[i+2] === 96) {
            finishBlocks();
            // The line with quotes is skipped entirely
            tonl();
            const start = i;
            while (! (bytes[i] === 96 && bytes[i+1] === 96 && bytes[i+2] === 96) && i < bytes.length) {
                tonl();
            }
            const end = i - 1;
            // The ending line is likewise skipped
            tonl();
            const text = dec.decode(bytes.slice(start, end));

            const pre = document.createElement("pre");
            pre.innerText = text;
            article.appendChild(pre);

        // * - list
        } else if (bytes[i] === 42) {
            // Finish the quote if exists
            if (currentQuote !== null) {
                article.appendChild(currentQuote);
                currentQuote = null;
            }
            // Create the current list if doesn't exist
            if (currentList === null) {
                currentList = document.createElement("ul");
            }

            i += 1; // Skip the asterisk
            ws();
            const text = textToNl();
            const li = document.createElement("li");
            li.innerText = text;
            currentList.appendChild(li);

        // > - quote
        } else if (bytes[i] === 62) {
            // Finish the list if exists
            if (currentList !== null) {
                article.appendChild(currentList);
                currentList = null;
            }
            // Create the current quote if doesn't exist
            if (currentQuote === null) {
                currentQuote = document.createElement("blockquote");
            }

            i += 1; // Skip the arrow
            ws();
            const text = textToNl();
            const p = document.createElement("p");
            p.innerText = text;
            currentQuote.appendChild(p);

        // None of the above - paragraph of text
        } else {
            finishBlocks();
            const text = textToNl()
            const p = document.createElement("p");
            p.innerText = text;
            article.appendChild(p);
        }
    }

    finishBlocks();

    return article;
}

/**
 * @param {Uint8Array} bytes
 * @returns {{text: string, contentType: string | null}}
 */
function displayHeader(bytes) {
    const dec = new TextDecoder();

    if (bytes.length < 3) {
        return {text: "<p> Invalid response </p>", contentType: null}
    }

    let code = String.fromCharCode(unwrap(bytes[0])) + String.fromCharCode(unwrap(bytes[1]));

    // 1X - Input required
    if (bytes[0] === 49) {
        let text;
        // 11 - Sensitive input
        if (bytes[1] === 49) {
            text = `<p>${code} Sensitive Input Required</p>`;
        } else {
            text = `<p>${code} Input Required</p>`;
        }
        const question = dec.decode(bytes.slice(3));

        return {
            text: text + "<p>" + question + "</p>",
            contentType: null,
        };

    // 2X - Success
    } else if (bytes[0] == 50) {
        const contentType = dec.decode(bytes.slice(3));
        return {
            text: `<p>${code} Success</p>`,
            contentType,
        };

    // 3X - Redirect
    } else if (bytes[0] == 51) {
        let text;
        // 30 - Temporary
        if (bytes[1] == 48) {
            text = `<p>${code} Temporary Redirect</p>`;
        // 31 - Permanent
        } else if (bytes[1] === 49) {
            text = `<p>${code} Permanent Redirect</p>`;
        } else {
            text = `<p>${code} Redirect</p>`;
        }

        const target = dec.decode(bytes.slice(3));
        return {
            text: text + `<a href="web+${target}"> ${target} </a>`,
            contentType: null,
        };

    // 4X - Temporary failure
    } else if (bytes[0] === 52) {
        let text;
        // 41 - unavailable
        if (bytes[1] === 49) {
            text = `<p>${code} Server Unavailable</p>`;
        // 42 - cgi
        } else if (bytes[1] === 50) {
            text = `<p>${code} CGI Error </p>`;
        // 43 - proxy
        } else if (bytes[1] === 51) {
            text = `<p>${code} Proxy Error </p>`;
        // 44 - slow down
        } else if (bytes[1] === 52) {
            text = `<p>${code} Too Many Requests </p>`;
        } else {
            text = `<p>${code} Temporary Server Failure </p>`;
        }
        const message = dec.decode(bytes.slice(3));
        text += "<p>" + message + "</p>";

        return { text, contentType: null };

    // 5X - Permanent failure
    } else if (bytes[0] === 53) {
        let text;
        // 51 - not found
        if (bytes[1] === 49) {
            text = `<p>${code} Not Found </p>`;
        // 52 - gone
        } else if (bytes[1] === 50) {
            text = `<p>${code} Resource No Longer Available </p>`;
        // 53 - proxy
        } else if (bytes[1] === 51) {
            text = `<p>${code} Proxy Request Refused </p>`;
        // 59 - bad request
        } else if (bytes[1] === 57) {
            text = `<p>${code} Bad Request </p>`;
        } else {
            text = `<p>${code} Permanent Server Failure </p>`;
        }
        const message = dec.decode(bytes.slice(3));
        text += "<p>" + message + "</p>";

        return {text, contentType: null};
    } else {
        return {text: "<p> Invalid response </p>", contentType: null}
    }
}

/**
 * @param {object} o
 * @returns {HTMLElement}
 */
function parseProgress(o) {
    const unexpected_message = "Fatal error: unexpected message from the native program";
    if (typeof o !== "object") {
        // Malformed message
        const div = document.createElement("div");
        div.innerText = unexpected_message;
        return div;
    }

    if ("error" in o && typeof o.error === "string") {
        /** @type {Record<string, string>}*/
        const errors = {
            "INTERNAL_INIT": "Internal error: initialization failed",
            "INTERNAL_SSL_CONFIG": "Internal error: SSL configuration failed",
            "INTERNAL_SSL_SETUP": "Internal error: SSL setup failed",
            "INTERNAL_SSL_HOSTNAME": "Internal error: SSL hostname binding failed",

            "NETWORK_FAILED": "Network unreachable",
            "LOOKUP_FAILED": "DNS record for the server returned empty",
            "CONNECT_FAILED": "Failed to connect to the server",
            "HANDSHAKE_FAILED": "TLS negotiation with the server failed",
            "REQUEST_FAILED": "Server didn't accept the request",
            "RESPONSE_FAILED": "Server didn't provide the response",
        };
        const div = document.createElement("div");
        div.innerText = errors[o.error] || unexpected_message;
        return div;
    } else if ("progress" in o && typeof o.progress === "string") {
        /** @type {Record<string, string>}*/
        const statuses = {
            "LOOKUP_DOMAIN": "Looking up DNS records...",
            "ESTABLISH_CONNECTION": "Connecting to the server...",
            "ESTABLISH_HANDSHAKE": "Establishing TLS with the server...",
            "SEND_REQUEST": "Sending the request...",
        };
        const div = document.createElement("div");
        if (o.progress.startsWith("RESPONSE_PARTIAL ")) {
            div.innerText = "Downloaded " + o.progress.slice(17) + " bytes...";
        } else {
            div.innerText = statuses[o.progress] || unexpected_message;
        }
        return div;
    } else {
        // Malformed message
        const div = document.createElement("div");
        div.innerText = unexpected_message;
        return div;
    }
}

/**
 * @param {object} message
 */
function displayProgress(message) {
    const progress = parseProgress(message);
    document.body.replaceChildren(progress);
}

/**
 * @param {string} responseB64
 * @param {string} currentLocation
 */
function displayResponse(responseB64, currentLocation) {
    const resp = Uint8Array.fromBase64(responseB64);

    // Find the end of the header by "\r\n"
    let i = 0;
    while (resp[i] != 13 && resp[i+1] != 10) {
        i += 1;
    }
    const header = resp.slice(0, i);
    const body = resp.slice(i+2);

    const headerRepr = displayHeader(header);
    if (headerRepr.contentType !== null) {
        if (headerRepr.contentType.startsWith("text/gemini")) {
            const article = parseGemtext(body, currentLocation);
            document.body.replaceChildren(article);
        } else {
            document.write("unknown content type: " + headerRepr.contentType);
        }
    } else {
        document.write(headerRepr.text);
    }
}

/**
 * @param {string} message
 */
function displayError(message) {
    const div = document.createElement("div");
    div.innerText = message;
    document.body.replaceChildren(div);
}

/**
 * @param {string} queryPart
 * @returns Promise<void>
 */
function navigateTo(queryPart) {
    if (!queryPart.startsWith("?")) {
        displayError("Navigate to a gemini:// url to start");
        return;
    }
    const webUrl = decodeURIComponent(queryPart.slice(1));
    if (!webUrl.startsWith("web+gemini://")) {
        displayError("Navigate to a gemini:// url to start");
        return;
    }
    const url = webUrl.slice(4);

    /**
     * @param {object} resp
     */
    function responseReceived(resp) {
        if (typeof resp === "string") {
            port.disconnect();
            displayResponse(resp, url);
        } else {
            displayProgress(resp);
        }
    }

    const port = browser.runtime.connect();
    port.onMessage.addListener(responseReceived);

    const messageArray = (new TextEncoder()).encode(url);
    const messageB64 = messageArray.toBase64();
    port.postMessage(messageB64);
}

console.log(document.body.lastElementChild);
console.log(document.body.lastElementChild?.nodeName);
if (document.body.lastElementChild instanceof HTMLElement && document.body.lastElementChild?.nodeName === "ARTICLE") {
    // A body with an already rendered page
    // Don't do anything, as this is a history navigation
    console.log("reusing cached page");
} else {
    // On initial load, navigate to the location
    // The manifest passes the location as a whole query part, it's url-encoded
    navigateTo(window.location.search);
}
