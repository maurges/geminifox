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


// The manifest passes the location after a sha-bang, it's url-encoded
const loc = decodeURIComponent(window.location.hash.slice(2)).slice(4);
// Todo: validate the location before passing it to native

/** Adjust a href of a created element to be openable with this extension
 * @param {string} href
 * @returns {string}
 */
function adjustHref(href) {
    const url = URL.parse(href, loc);
    if (url !== null && url.protocol === "gemini:") {
        return "web+" + url.toString();
    } else {
        return href;
    }
}

/**
 * @param {Uint8Array} bytes
 * @returns HTMLElement
 */
function parseGemtext(bytes) {
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
        i = r + 1;
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
            const textStart = i;
            const textEnd = tonl();
            const text = textStart === textEnd ? href : dec.decode(bytes.slice(textStart, textEnd));

            const container = document.createElement("p");
            const a = document.createElement("a");
            a.href = adjustHref(href);
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
            while (! (bytes[i] === 96 && bytes[i+1] === 96 && bytes[i+2] === 96)) {
                i = bytes.indexOf(96, i + 1);
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
            const start = i;
            const end = tonl();
            const text = dec.decode(bytes.slice(start, end));

            const p = document.createElement("p");
            p.innerText = text;
            article.appendChild(p);
        }
    }

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
 * @param {string} loc
 * @returns Promise<void>
 */
async function run(loc) {
    /**
     * @param {object} responseB64
     */
    function responseReceived(responseB64) {
        if (!(typeof responseB64 === "string")) {
            console.log("status response", responseB64);
            return;
        }

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
                const article = parseGemtext(body);
                document.body.appendChild(article);
            } else {
                document.write("unknown content type: " + headerRepr.contentType);
            }
        } else {
            document.write(headerRepr.text);
        }
    }

    const port = browser.runtime.connect();
    port.onMessage.addListener(responseReceived);

    const messageArray = (new TextEncoder()).encode(loc);
    const messageB64 = messageArray.toBase64();
    port.postMessage(messageB64);
}

run(loc)
