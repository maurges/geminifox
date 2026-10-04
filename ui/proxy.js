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
function unwrapUnchecked(x) {
    return /** @type A */ (x);
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
            while (bytes[i] != 32 && bytes[i] != 9 && bytes[i] != 10 && i < bytes.length) {
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
 * @returns {{code: number, text: string} | null}
 */
function parseHeader(bytes) {
    if (bytes.length < 3) {
        return null;
    }
    const code10 = unwrapUnchecked(bytes[0]);
    const code1 = unwrapUnchecked(bytes[1]);
    if (code10 <= 48 || code10 > 57 || code1 < 48 || code1 > 57) {
        return null;
    }

    const code = (code10 - 48) * 10 + code1 - 48;
    const textBs = bytes.slice(3);
    const text = (new TextDecoder()).decode(textBs);
    return {code, text};
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
    while (resp[i] != 13 && resp[i+1] != 10 && i < resp.length) {
        i += 1;
    }
    const header = resp.slice(0, i);
    const body = resp.slice(i+2);

    const headerRepr = parseHeader(header);
    if (headerRepr === null) {
        const div = document.createElement("div");
        div.innerText = "Malformed server reply";
        console.log(header);
        document.body.replaceChildren(div);
        return
    };
    const {code, text} = headerRepr;

    console.log("response", code, text);

    // 1X - Input required
    if (code >= 10 && code <= 19) {
        const div = document.createElement("div");
        const notice = document.createElement("div");
        notice.innerText = code === 11 ? "Sensitive input required" : "Input required";
        const request = document.createElement("div");
        request.innerText = text;
        div.appendChild(notice);
        div.appendChild(request);
        document.body.replaceChildren(div);

    // 2X - Success
    } else if (code >= 20 && code <= 29) {
        if (text.startsWith("text/gemini")) {
            const article = parseGemtext(body, currentLocation);
            document.body.replaceChildren(article);
        } else {
            const div = document.createElement("div");
            div.innerText = "Unknown content type: " + text;
            document.body.replaceChildren(div);
        }

    // 3X - Redirect
    } else if (code >= 30 && code <= 39) {
        const div = document.createElement("div");
        const notice = document.createElement("div");
        notice.innerText =
            code === 30 ? "Temporary redirect" :
            code === 31 ? "Permanent redirect" :
            "Redirect";
        const target = document.createElement("a");
        target.href = text;
        target.innerText = text;
        div.appendChild(notice);
        div.appendChild(target);
        document.body.replaceChildren(div);

    // 4X - Temporary failure
    } else if (code >= 40 && code <= 49) {
        const div = document.createElement("div");
        div.innerText =
            code === 41 ? "41 Server Unavailable" :
            code === 42 ? "42 CGI Error" :
            code === 43 ? "43 Server Proxy Error" :
            code === 44 ? "44 Too Many Requests" :
            `${code} Temporary Server Failure`;
        document.body.replaceChildren(div);

    // 5X - Permanent failure
    } else if (code >= 50 && code <= 59) {
        const div = document.createElement("div");
        div.innerText =
            code === 51 ? "51 Not Found" :
            code === 52 ? "52 Resource No Longer Available" :
            code === 53 ? "53 Server Proxy Request Refused" :
            code === 59 ? "59 Bad Request" :
            `${code} Permanent Server Failure`;
        document.body.replaceChildren(div);

    // Unknown status
    } else {
        const div = document.createElement("div");
        div.innerText = "Malformed server reply";
        document.body.replaceChildren(div);
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
        displayError(`Unknown protocol in '${webUrl}'. Navigate to a gemini:// url to start`);
        return;
    }
    const url = webUrl.slice(4);

    /**
     * @param {object} resp
     */
    function responseReceived(resp) {
        if (typeof resp === "string") {
            port.disconnect();
            // Remember the fetched result
            history.replaceState({gemfox: {resp, url}}, "");
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

// Check if this page is in the history
const wasReload = performance.getEntriesByType("navigation")[0]?.type === "reload";
if (wasReload) {
    console.log("clear history on refresh");
    history.replaceState(null);
    navigateTo(window.location.search);
} else if (history.state !== null && "gemfox" in history.state) {
    const oldState = history.state.gemfox;
    if (
        "resp" in oldState
        && typeof oldState.resp === "string"
        && "url" in oldState
        && typeof oldState.url === "string"
    ) {
        console.log("restored from history");
        displayResponse(oldState.resp, oldState.url);
    } else{
        console.error("invalid history item", oldState);
        navigateTo(window.location.search);
    }
} else {
    // On initial load, navigate to the location
    // The manifest passes the location as a whole query part, it's url-encoded
    navigateTo(window.location.search);
}
