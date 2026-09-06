// Every emoji the interface shows carries a tooltip explaining it. The chips in the type picker
// are a FILTER, so a reader who does not know the vocabulary cannot tell what narrows the list;
// the tooltip is what makes each one self-describing. A label table is written by hand while the
// emoji themselves are declared across ~90 module headers, so the two drift apart silently: what
// this pins is that no emoji reaches a user without an explanation.
// Run: `node --test test/js/ui-emoji-labels.test.mjs`.

import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { execSync } from "node:child_process";

const APP = new URL("../../src/ui/app.js", import.meta.url).pathname;
const src = readFileSync(APP, "utf8");

/// app.js is a browser script rather than a module, so the vocabularies are lifted out of it by
/// name and evaluated. Reading the source is the point: a test that redeclared the tables would
/// pass while the shipped ones drifted.
function tableFrom(name) {
    const i = src.indexOf(`const ${name}`);
    assert.ok(i >= 0, `${name} not found in app.js`);
    const j = src.indexOf("\n};", i);
    return src.slice(i, j + 3);
}
function constFrom(name) {
    const i = src.indexOf(`const ${name}`);
    assert.ok(i >= 0, `${name} not found in app.js`);
    // To the statement's semicolon, not to the end of the line: these declarations carry a
    // trailing comment, and cutting at the newline truncates the value itself.
    return src.slice(i, src.indexOf(";", i) + 1);
}

const SCRIPTED_EMOJI = "\u{1F4DD}";
const COMPILED_EMOJI = "\u{1F4E6}";
const EMOJI_LABEL = new Function("SCRIPTED_EMOJI", "COMPILED_EMOJI",
    tableFrom("EMOJI_LABEL") + "; return EMOJI_LABEL;")(SCRIPTED_EMOJI, COMPILED_EMOJI);

const seg = new Intl.Segmenter(undefined, { granularity: "grapheme" });
const emojiIn = (s) => [...seg.segment(s)].map(g => g.segment).filter(g => g.trim());

test("every emoji the interface generates has a tooltip", () => {
    const ROLE_EMOJI = new Function(tableFrom("ROLE_EMOJI") + "; return ROLE_EMOJI;")();
    const DIM_EMOJI = new Function(tableFrom("DIM_EMOJI") + "; return DIM_EMOJI;")();
    const KERNEL_EMOJI = new Function(constFrom("KERNEL_EMOJI") + "; return KERNEL_EMOJI;")();

    const shown = [...Object.values(ROLE_EMOJI), ...Object.values(DIM_EMOJI),
                   ...KERNEL_EMOJI, SCRIPTED_EMOJI, COMPILED_EMOJI];
    const missing = shown.filter(e => !EMOJI_LABEL[e]);
    assert.deepEqual(missing, [], `emoji shown with no tooltip: ${missing.join(" ")}`);
});

test("every emoji a module declares in tags() has a tooltip", () => {
    // The modules are the moving half: a new effect adds a tag emoji, and nothing else would
    // notice that the picker then shows a character no tooltip explains.
    const out = execSync(
        `grep -rho 'const char\\* tags() const override { return "[^"]*"' src/ || true`,
        { encoding: "utf8", cwd: new URL("../..", import.meta.url).pathname });
    const declared = new Set();
    for (const line of out.split("\n")) {
        const m = line.match(/return "([^"]*)"/);
        if (m) for (const e of emojiIn(m[1])) declared.add(e);
    }
    assert.ok(declared.size > 10, `expected the module headers to declare emoji, found ${declared.size}`);
    const missing = [...declared].filter(e => !EMOJI_LABEL[e]);
    assert.deepEqual(missing, [], `declared in tags() with no tooltip: ${missing.join(" ")}`);
});
