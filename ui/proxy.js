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
 * @returns {{article: HTMLElement, title: string | null }}
 */
function parseGemtext(bytes, currentLocation) {
    const dec = new TextDecoder();

    const article = document.createElement("article");
    let title = null;

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

            // Set the first found header as title
            if (title === null) {
                title = text;
            }

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

    return {article, title};
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
        console.log("unexpected message type", o);
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

            "NATIVE_FAILED": "Failed to run the native program",

            "NETWORK_FAILED": "Network unreachable",
            "LOOKUP_FAILED": "DNS record for the server returned empty",
            "CONNECT_FAILED": "Failed to connect to the server",
            "HANDSHAKE_FAILED": "TLS negotiation with the server failed",
            "REQUEST_FAILED": "Server didn't accept the request",
            "RESPONSE_FAILED": "Server didn't provide the response",
        };
        const div = document.createElement("div");
        const text = errors[o.error];
        if (text) {
            div.innerText = text;
        } else {
            console.log("unexpected error", o);
            div.innerText = unexpected_message;
        }
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
            const text = statuses[o.progress];
            if (text) {
                div.innerText = text;
            } else {
                console.log("unexpected progress", o);
                div.innerText = unexpected_message;
            }
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
 * @param {number} code
 * @param {string} text - header text
 * @param {Uint8Array} body
 * @param {string} currentLocation
 * @returns {{body: HTMLElement, title?: string | null, redirect?: string}}
 */
function displayResponse(code, text, body, currentLocation) {
    // 1X - Input required
    if (code >= 10 && code <= 19) {
        const div = document.createElement("div");
        const notice = document.createElement("div");
        notice.innerText = code === 11 ? "Sensitive input required" : "Input required";
        const request = document.createElement("div");
        request.innerText = text;
        div.appendChild(notice);
        div.appendChild(request);
        return {body: div};

    // 2X - Success
    } else if (code >= 20 && code <= 29) {
        if (text.startsWith("text/gemini")) {
            const {article, title} = parseGemtext(body, currentLocation);
            return {body: article, title};
        } else if (text.startsWith("text/plain")) {
            const article = document.createElement("pre");
            article.innerText = (new TextDecoder()).decode(body);
            return {body: article};
        } else {
            const div = document.createElement("div");
            div.innerText = "Unknown content type: " + text;
            return {body: div};
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
        const href = adjustHref(text, currentLocation);
        target.href = href;
        target.innerText = href;
        div.appendChild(notice);
        div.appendChild(target);
        return {body: div, redirect: text};

    // 4X - Temporary failure
    } else if (code >= 40 && code <= 49) {
        const div = document.createElement("div");
        div.innerText =
            code === 41 ? "41 Server Unavailable" :
            code === 42 ? "42 CGI Error" :
            code === 43 ? "43 Server Proxy Error" :
            code === 44 ? "44 Too Many Requests" :
            `${code} Temporary Server Failure`;
        return {body: div};

    // 5X - Permanent failure
    } else if (code >= 50 && code <= 59) {
        const div = document.createElement("div");
        div.innerText =
            code === 51 ? "51 Not Found" :
            code === 52 ? "52 Resource No Longer Available" :
            code === 53 ? "53 Server Proxy Request Refused" :
            code === 59 ? "59 Bad Request" :
            `${code} Permanent Server Failure`;
        return {body: div};

    // Unknown status
    } else {
        const div = document.createElement("div");
        div.innerText = "Malformed server reply";
        return {body: div};
    }
}

/**
 * @param {string} responseB64
 * @param {string} currentLocation
 * @param {string[]} redirects
 */
function handleResponse(responseB64, currentLocation, redirects) {
    const resp = Uint8Array.fromBase64(responseB64);

    // Find the end of the header by "\r\n"
    let i = 0;
    while (resp[i] != 13 && resp[i+1] != 10 && i < resp.length) {
        i = resp.indexOf(13, i);
        if (i === -1) {
            i = resp.length;
        }
    }
    const header = resp.slice(0, i);
    const body = resp.slice(i+2);

    const headerRepr = parseHeader(header);
    if (headerRepr === null) {
        const div = document.createElement("div");
        div.innerText = "Malformed server reply";
        document.body.replaceChildren(div);
        return;
    };

    const rendered = displayResponse(headerRepr.code, headerRepr.text, body, currentLocation);

    if (rendered.title) {
        document.title = rendered.title;
    }

    if (rendered.redirect) {
        redirects.push(rendered.redirect);
        // First do a loop check
        if (redirects.length == 15) {
            const uniqs = new Set(redirects);
            if (uniqs.size < 15) {
                const div = document.createElement("div");
                const p = document.createElement("p");
                p.innerText = "Redirect loop detected. Trace:";
                div.appendChild(p);
                const ul = document.createElement("ul");
                for (const href of redirects) {
                    const li = document.createElement("li");
                    li.innerText = href;
                    ul.appendChild(li);
                }
                div.appendChild(ul);
                document.body.replaceChildren(div);
            // No loops, continue redirecting
            } else {
                navigateTo(rendered.redirect, redirects);
            }
        // Abort if too many redirects
        } else if (redirects.length > 30) {
            const div = document.createElement("div");
            const p = document.createElement("p");
            p.innerText = "Too many redirects. Trace:";
            div.appendChild(p);
            const ul = document.createElement("ul");
            for (const href of redirects) {
                const li = document.createElement("li");
                li.innerText = href;
                ul.appendChild(li);
            }
            div.appendChild(ul);
            document.body.replaceChildren(div);
        // Redirect to the new page
        } else {
            navigateTo(rendered.redirect, redirects);
        }
    // No redirects, render the page
    } else {
        document.body.replaceChildren(rendered.body);
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

/** Load and display a gemini url
 * @param {string} url
 * @param {string[]} redirects
 */
function navigateTo(url, redirects) {
    document.title = url;

    /**
     * @param {object} resp
     */
    function responseReceived(resp) {
        if (typeof resp === "string") {
            port.disconnect();
            // Remember the fetched result
            history.replaceState({geminifox: {resp, url}}, "", "proxy.html?web+" + url);
            handleResponse(resp, url, redirects);
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

/** Open a url provided by the query param to the extension
 * @param {string} queryPart
 */
function openPage(queryPart) {
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
    navigateTo(url, []);
}

// Check if we've been on this page before
/** @ts-ignore */
const wasReload = performance.getEntriesByType("navigation")[0]?.type === "reload";
if (wasReload) {
    history.replaceState(null, "");
    openPage(window.location.search);
} else if (history.state !== null && "geminifox" in history.state) {
    const oldState = history.state.geminifox;
    if (
        "resp" in oldState
        && typeof oldState.resp === "string"
        && "url" in oldState
        && typeof oldState.url === "string"
    ) {
        handleResponse(oldState.resp, oldState.url, []);
    } else{
        console.error("invalid history item", oldState);
        openPage(window.location.search);
    }
} else {
    // On initial load, navigate to the location
    // The manifest passes the location as a whole query part, it's url-encoded
    openPage(window.location.search);
}
