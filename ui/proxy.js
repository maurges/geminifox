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
 * @returns {{article: HTMLElement, title: string | undefined }}
 */
function parseGemtext(bytes, currentLocation) {
    const dec = new TextDecoder();

    const article = document.createElement("article");
    let title = undefined;

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
        return dec.decode(bytes.subarray(start, end));
    }

    /** @type {null | HTMLElement} */
    let currentList = null;
    /** @type {null | HTMLElement} */
    let currentQuote = null;
    /** @type {null | HTMLElement} */
    let currentLinks = null;
    /** @param {"list"|"quote"|"links"|undefined} [thisBlock] */
    function finishBlocks(thisBlock) {
        if (thisBlock !== "list" && currentList !== null) {
            article.appendChild(currentList);
            currentList = null;
        }
        if (thisBlock !== "quote" && currentQuote !== null) {
            article.appendChild(currentQuote);
            currentQuote = null;
        }
        if (thisBlock !== "links" && currentLinks !== null) {
            article.appendChild(currentLinks);
            currentLinks = null;
        }
    }

    while (i < bytes.length) {
        // Start of a line

        // => - link
        if (bytes[i] == 61 && bytes[i+1] == 62) {
            finishBlocks("links");
            // Create the current block if doesn't exist
            if (currentLinks === null) {
                currentLinks = document.createElement("div");
                currentLinks.classList.add("link-group");
            }

            i += 2; // skip the arrow
            ws();
            const hrefStart = i;
            // Url is delimited by whitespace
            while (bytes[i] != 32 && bytes[i] != 9 && bytes[i] != 10 && i < bytes.length) {
                i += 1;
            }
            const hrefEnd = i;
            const href = dec.decode(bytes.subarray(hrefStart, hrefEnd));
            ws();
            const mbText = textToNl();
            const text = mbText.length === 0 ? href : mbText;

            const a = document.createElement("a");
            a.href = adjustHref(href, currentLocation);
            a.innerText = text;
            currentLinks.appendChild(a);

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
            const text = dec.decode(bytes.subarray(start, end));

            const pre = document.createElement("pre");
            pre.innerText = text;
            article.appendChild(pre);

        // * - list
        } else if (bytes[i] === 42) {
            finishBlocks("list");
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
            finishBlocks("quote");
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
    const textBs = bytes.subarray(3);
    const text = (new TextDecoder()).decode(textBs);
    return {code, text};
}

/**
 * @param {string} title
 * @param {(string | HTMLElement | {text: string, link: string})[]} messages
 * @param {string} className
 * @param {boolean} reload
 * @returns HTMLElement
 */
function renderStatusPage(title, messages, className, reload) {
    const div = document.createElement("div");
    div.classList.add(className);
    div.innerText = title;

    for (const msg of messages) {
        if (msg instanceof HTMLElement) {
            div.appendChild(msg);
        } else if (typeof msg === "string") {
            if (msg.length === 0) {
                // Sometimes I emit empty strings for convenience
                continue;
            }
            const p = document.createElement("p");
            p.innerText = msg;
            div.appendChild(p);
        } else {
            const p = document.createElement("p");
            const a = document.createElement("a");
            a.href = msg.link;
            a.innerText = msg.link;
            p.append(msg.text, a);
            div.append(p);
        }
    }

    if (reload) {
        const reloadButton = document.createElement("button");
        reloadButton.innerText = "Reload";
        reloadButton.onclick = () => window.location.reload();
        div.appendChild(reloadButton);
    }

    return div;
}

/**
 * @param {object} o
 * @param {number} downloadedSize
 * @returns {{show?: HTMLElement, append?: Uint8Array<ArrayBuffer>, success?: boolean}}
 */
function parseProgress(o, downloadedSize) {
    console.log("parse progress", o);
    const retryButton = document.createElement("button");
    retryButton.innerText = "Reload";
    retryButton.onclick = () => window.location.reload();

    const ourProblem = [
        {
            text: "The error is in the native program used to access the remote pod. If you can reproduce it, please report it to us at ",
            link:  "https://github.com/maurges/geminifox",
        },
        "version" in o ? `The program version is ${o.version}` : "",
    ];

    const unexpected_message = "Internal error: unexpected message from the native program";
    if (typeof o !== "object") {
        // Malformed message
        console.log("unexpected message type", o);
        return {show: renderStatusPage(
            "Internal error: unexpected message from the native program",
            ourProblem,
            "error",
            true,
        )};
    }

    if ("error" in o && typeof o.error === "string") {
        const internalError = (/** @type {string} */ title) => () => renderStatusPage(title, ourProblem, "error", true);
        const remoteError = (/** @type {string} */ title) => () => renderStatusPage(
            title,
            ["The remote host is unreachable. Maybe it doesn't exist anymore, maybe your network has a problem"],
            "error",
            true
        );
        /** @type {Record<string, () => HTMLElement>} */
        const errors = {
            "INTERNAL_INIT": internalError("Internal error: initialization failed"),
            "INTERNAL_SSL_CONFIG": internalError("Internal error: SSL configuration failed"),
            "INTERNAL_SSL_SETUP": internalError("Internal error: SSL setup failed"),
            "INTERNAL_SSL_HOSTNAME": internalError("Internal error: SSL hostname binding failed"),
            "INTERNAL_MKDIR": internalError("Internal error: failed to create data directory"),
            "INTERNAL_PATH_SIZE": internalError("Internal error: the URL + tmpdir path is too large"),
            "INTERNAL_CERT_NAME": internalError("Internal error: failed to set name for the SSL certificate"),
            "INTERNAL_CERT_BYTES": internalError("Internal error: SSL certificate can't be produced"),
            "INTERNAL_WRITE": internalError("Internal error: failed to write persistent data"),
            "INTERNAL_COMMAND_READ": internalError("Internal error: failed to read the command from firefox"),
            "INTERNAL_COMMAND_PARSE": internalError("Internal error: failed to parse the command from firefox"),

            "NATIVE_FAILED": () => renderStatusPage(
                "Failed to run the native program",
                [
                    {
                        text: "Did you forget to install the native program? Download it here: ",
                        link: "https://github.com/maurges/geminifox",
                    },
                    "If you're on macos, this failure can randomly happen 1 in 5 loads. "
                        + "Please refresh the page",
                    ...ourProblem
                ],
                "error",
                true,
            ),

            "LOOKUP_FAILED": remoteError("DNS record for the server returned empty"),
            "CONNECT_FAILED": remoteError("Failed to connect to the server"),
            "HANDSHAKE_FAILED": remoteError("TLS negotiation with the server failed"),
            "REQUEST_FAILED": remoteError("Server didn't accept the request"),
            "RESPONSE_FAILED": remoteError("Server didn't provide the response"),
            "BAD_URL": () => renderStatusPage(
                "Bad URL",
                ["The URL you supplied is invalid for accessing gemini pods"],
                "error",
                false
            ),
            "NETWORK_FAILED": () => renderStatusPage(
                "Network unreachable",
                ["There seems to be a network issue on your machine. Fix it and try again later"],
                "error",
                true,
            ),
            "KEY_MALFORMED": () => renderStatusPage(
                "Client certificate malformed",
                [
                    "You have previously selected to save a client certificate for this pod, "
                        + "and this certificate has become corrupted. "
                        + "This might be a disk error or another process fiddling the files",
                    ...ourProblem
                ],
                "error",
                true,
            ),
        };
        const page = errors[o.error];
        if (page) {
            return {show: page()}
        } else {
            console.log("unexpected error", o);
            return {show: renderStatusPage(
                unexpected_message,
                ourProblem,
                "error",
                true,
            )};
        }
    } else if ("progress" in o && typeof o.progress === "string") {
        /** @type {Record<string, string>}*/
        const statuses = {
            "LOOKUP_DOMAIN": "Looking up DNS records...",
            "ESTABLISH_CONNECTION": "Connecting to the server...",
            "ESTABLISH_HANDSHAKE": "Establishing TLS with the server...",
            "SEND_REQUEST": "Sending the request...",
            "KEY_LOADED": "Loaded the client TLS certificate...",
            "KEY_CREATED": "Created the client TLS certificate!",
        };
        const text = statuses[o.progress];
        // A bit of a hack, maybe I'll want to transmit success messages some other way. For fetch too
        const success = o.progress === "KEY_CREATED";
        if (text) {
            return {show: renderStatusPage(text, [], "progress", false), success};
        } else {
            return {show: renderStatusPage(unexpected_message, ourProblem, "progress", false)}
        }
    } else if ("part" in o && typeof o.part === "string") {
        try {
            const resp = Uint8Array.fromBase64(o.part);
            const text = "Downloaded " + (downloadedSize + resp.length).toString() + " bytes";
            return {
                append: resp,
                show: renderStatusPage(text, [], "progress", false),
            };
        } catch (e) {
            // Malformed message
            return {show: renderStatusPage(unexpected_message, ourProblem, "progress", false)}
        }
    } else {
        // Malformed message
        return {show: renderStatusPage(unexpected_message, ourProblem, "error", true)}
    }
}

/**
 * @param {number} code
 * @param {string} text - header text
 * @param {Uint8Array<ArrayBuffer>} body
 * @param {string} currentLocation
 * @returns {{body: HTMLElement, title?: string | undefined, redirect?: string}}
 */
function displayResponse(code, text, body, currentLocation) {
    // 1X - Input required
    if (code >= 10 && code <= 19) {
        const div = document.createElement("div");

        const notice = document.createElement("div");
        notice.innerText = code === 11 ? "Sensitive input required" : "Input required";
        div.appendChild(notice);

        const form = document.createElement("form");

        const label = document.createElement("label");
        label.innerText = text;
        form.appendChild(label);

        const input = document.createElement("input");
        input.type = "text";
        form.appendChild(input);

        const button = document.createElement("button");
        button.innerText = "input";
        button.type = "submit";
        form.appendChild(button);

        form.addEventListener("submit", e => {
            e.preventDefault();

            const url = expect(URL.parse(currentLocation), "url failed to parse");
            url.search = input.value;
            url.protocol = "web+" + url.protocol;
            document.location.search = url.toString();
        });

        div.appendChild(form);


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
        } else if (text.startsWith("image/")) {
            const blob = new Blob([body], { type: text });
            const blobUrl = URL.createObjectURL(blob);
            const img = document.createElement("img");
            img.src = blobUrl;
            img.onload = () => { URL.revokeObjectURL(blobUrl) };
            return {body: img};
        } else if (text.startsWith("video/")) {
            const blob = new Blob([body], { type: text });
            const blobUrl = URL.createObjectURL(blob);
            const video = document.createElement("video");
            video.src = blobUrl;
            video.controls = true;
            video.onload = () => { URL.revokeObjectURL(blobUrl) };
            return {body: video};
        } else {
            return {body: renderStatusPage(
                "Unknown content type: " + text,
                ["The server provided a response that this program can't show"],
                "error",
                false,
            )};
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
        const title =
            code === 41 ? "41 Server Unavailable" :
            code === 42 ? "42 CGI Error" :
            code === 43 ? "43 Server Proxy Error" :
            code === 44 ? "44 Too Many Requests" :
            `${code} Temporary Server Failure`;
        return {body: renderStatusPage(
            title,
            ["The remote pod responded with the temporary error. Try again later"],
            "error",
            true,
        )};

    // 5X - Permanent failure
    } else if (code >= 50 && code <= 59) {
        const title =
            code === 51 ? "51 Not Found" :
            code === 52 ? "52 Resource No Longer Available" :
            code === 53 ? "53 Server Proxy Request Refused" :
            code === 59 ? "59 Bad Request" :
            `${code} Permanent Server Failure`;
        return {body: renderStatusPage(
            title,
            ["The remote pod responded with the permanent error. Change your request or contact the pod administrator"],
            "error",
            true,
        )};

    // 6X - Certificate required
    } else if (code >= 60 && code <= 69) {
        if (code === 61) {
            return {body: renderStatusPage(
                "61 Certificate not authorized",
                ["The content at this location is not made available to you by the pod administrators"],
                "error",
                false,
            )};
        }

        const title =
            code === 62 ? "62 Certificate Not Valid" :
            `${code} Certificate Required`;

        const enableButton = document.createElement("button");
        enableButton.innerText = "create certificate";
        enableButton.onclick = () => createKey(currentLocation);
        const enablePrompt = document.createElement("p");
        enablePrompt.append(
            "Click this button to proceed with a new certificate: ",
            enableButton,
        );
        return {body: renderStatusPage(
            title,
            [
                "The content you're trying to access requires a client certificate. "
                    + "Enabling the certificate will allow this pod to recognize you",
                enablePrompt,
                "Leave this page if you want to remain anonymous on this pod",
            ],
            "progress",
            false,
        )}

    // Unknown status
    } else {
        return {body: renderStatusPage(
            "Malformed server reply with unknown status code " + code.toString(),
            ["The remote pod reponded in breaking the expected protocol. Contact the pod administrator"],
            "error",
            true
        )};
    }
}

/** Compute the width of the element if it were rendered with "internal--measure" class
 * @param {HTMLElement} elem
 * @returns {number}
 */
function elementRenderedWidth(elem) {
    // Cache the computed width directly on the element, as the elements are static
    if (elem.dataset["renderedWidth"] === undefined) {
        elem.classList.add("internal--measure");
        const r = elem.getBoundingClientRect().width;
        elem.dataset["renderedWidth"] = r.toString();
        elem.classList.remove("internal--measure");
        return r;
    } else {
        const r = parseInt(elem.dataset["renderedWidth"]);
        if (Number.isNaN(r)) {
            panic("Stored 'renderedWidth' is not a number");
        }
        return r;
    }
}

// Will be set on the article after its render into html, and be triggered on
// resizes of the window
const widthObserver = new ResizeObserver(evs => {
    for (const ev of evs) {
        const article = /** @type HTMLElement */ (ev.target);
        const linkGroups = article.getElementsByClassName("link-group");
        // Tsc doesn't allow me to iterate with the for-of syntax?
        for (let i = 0; i < linkGroups.length; ++i) {
            // Being a bit sloppy here with the types. As if there is something
            // other than html elements. This also discards an undefined which
            // I'm pretty sure can't be in the middle of a queried list
            const linkGroup = /** @type HTMLElement */ (linkGroups[i]);
            // Set to vertical if the horizontal width is larger than available
            const renderedWidth = elementRenderedWidth(linkGroup);
            linkGroup.classList.toggle(
                "vertical",
                // Add the body padding, as it seems to not be taken into account
                renderedWidth + 10 > article.clientWidth
            )
        }
    }
});

/**
 * @param {Uint8Array<ArrayBuffer>} resp
 * @param {string} currentLocation
 * @param {string[]} redirects
 */
function handleResponse(resp, currentLocation, redirects) {
    // Find the end of the header by "\r\n"
    let i = 0;
    while (resp[i] != 13 && resp[i+1] != 10 && i < resp.length) {
        i = resp.indexOf(13, i);
        if (i === -1) {
            i = resp.length;
        }
    }
    const header = resp.subarray(0, i);
    const body = resp.subarray(i+2);

    const headerRepr = parseHeader(header);
    if (headerRepr === null) {
        const errpage = renderStatusPage(
            "Malformed server reply: can't parse header",
            ["The remote pod reponded in breaking the expected protocol. Contact the pod administrator"],
            "error",
            true,
        )
        document.body.replaceChildren(errpage);
        return;
    };

    const rendered = displayResponse(headerRepr.code, headerRepr.text, body, currentLocation);

    if (rendered.title) {
        document.title = rendered.title;
    }

    if (rendered.redirect) {
        redirects.push(rendered.redirect);
        // First do a loop check
        const renderLoopError = () => {
            const ul = document.createElement("ul");
            for (const href of redirects) {
                const li = document.createElement("li");
                li.innerText = href;
                ul.appendChild(li);
            }
            return renderStatusPage(
                "Redirect loop detected",
                [
                    "Redirect trace:",
                    ul,
                    "The remote pod is likely misconfigured. Cotact its administrator"
                ],
                "error",
                true,
                );
        };
        if (redirects.length == 15) {
            const uniqs = new Set(redirects);
            if (uniqs.size < 15) {
                document.body.replaceChildren(renderLoopError());
            // No loops, continue redirecting
            } else {
                navigateTo(rendered.redirect, redirects);
            }
        // Abort if too many redirects
        } else if (redirects.length > 30) {
            document.body.replaceChildren(renderLoopError());
        // Redirect to the new page
        } else {
            navigateTo(rendered.redirect, redirects);
        }
    // No redirects, render the page
    } else {
        // The currentLocation is a gemini url, but we want to remember the
        // prefixed version in history for firefox to navigate properly
        const fixedLocation = currentLocation.startsWith("web+")
            ? currentLocation : "web+" + currentLocation;
        browser.history.addUrl({
            url: fixedLocation,
            title: rendered.title,
        })
        document.body.replaceChildren(rendered.body);
        widthObserver.observe(rendered.body);
    }
}

/** Load and display a gemini url
 * @param {string} url
 * @param {string[]} redirects
 */
function navigateTo(url, redirects) {
    document.title = url;

    /** @type {Uint8Array<ArrayBuffer>[]} */
    const responseChunks = [];
    let responseSize = 0;

    /**
     * @param {object} resp
     */
    function responseReceived(resp) {
        const r = parseProgress(resp, responseSize);
        if ("show" in r) {
            document.body.replaceChildren(r.show);
        }
        if ("append" in r) {
            responseChunks.push(r.append);
            responseSize += r.append.length;
        }
    }

    function responseFinished() {
        if (responseSize === 0) {
            // Did not get a response. There must have been an error that we
            // receive before, don't even attempt to render the (empty) body
            return;
        }
        // Concatenate the chunks (low level language yeah)
        const fullResponse = new Uint8Array(responseSize);
        let offset = 0;
        for (const chunk of responseChunks) {
            fullResponse.set(chunk, offset);
            offset += chunk.length;
        }

        // Remember the fetched result
        try {
            history.replaceState({geminifox: {resp: fullResponse, url}}, "", "proxy.html?web+" + url);
        } catch (e) {
            console.log("Failed to replace history state", e);
            history.replaceState(null, "", "proxy.html?web+" + url);
        }
        handleResponse(fullResponse, url, redirects);
    }

    const port = browser.runtime.connect();
    port.onMessage.addListener(responseReceived);
    port.onDisconnect.addListener(responseFinished);

    const command = "fetch " + url;
    const messageArray = (new TextEncoder()).encode(command);
    const messageB64 = messageArray.toBase64();
    console.log("fetching", messageB64);
    port.postMessage(messageB64);
}

/** Ask the native program to create the key
 * @param {string} requestUrl - url for which this key is requested, of which only domain matters
 */
function createKey(requestUrl) {
    const port = browser.runtime.connect();
    let didSucceed = false;
    /**
     * @param {object} resp
     */
    function responseReceived(resp) {
        const r = parseProgress(resp, 0);
        if ("show" in r) {
            document.body.replaceChildren(r.show);
        }
        if ("success" in r && r.success) {
            didSucceed = true;
        }
    }
    function responseFinished() {
        if (didSucceed) {
            // Now open the target page and show it
            navigateTo(requestUrl, []);
        }
        // Didn't succeed. There must have been an error that we receive before
        // and are currently showing
    }
    port.onMessage.addListener(responseReceived);
    port.onDisconnect.addListener(responseFinished);

    const command = "keygen " + requestUrl;
    const messageArray = (new TextEncoder()).encode(command);
    const messageB64 = messageArray.toBase64();
    port.postMessage(messageB64);
}

/** Open a url provided by the query param to the extension
 * @param {string} queryPart
 */
function openPage(queryPart) {
    if (!queryPart.startsWith("?")) {
        document.body.replaceChildren(renderStatusPage(
            "Navigate to a gemini:// url to start",
            [],
            "progress",
            false,
        ));
        return;
    }
    const webUrl = decodeURIComponent(queryPart.slice(1));
    if (!webUrl.startsWith("web+gemini://")) {
        document.body.replaceChildren(renderStatusPage(
            `Unknown protocol in '${webUrl}'`,
            ["Navigate to a gemini:// url to start"],
            "error",
            false,
        ));
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
        && oldState.resp instanceof Uint8Array
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
