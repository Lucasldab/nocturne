# Friction Log Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let the user log a missing feature ("gap") from the phone or the terminal, and give Claude Code a PM agent that turns those gaps plus the code into ranked proposals.

**Architecture:** The phone appends one JSONL line per gap to `gaps-phone-<deviceid>.jsonl` in the Syncthing meta folder, using the existing `JsonlFileWriter`. A shell script appends desktop gaps to `gaps-desktop.jsonl` on hearth and lists all of them. nocturned never reads either file. A project subagent reads the gaps and writes only `docs/gaps-triage.md`.

**Tech Stack:** Kotlin, Jetpack Compose (Material 3), kotlinx.serialization, JUnit/Robolectric; POSIX sh + jq; bash tests; Claude Code subagent markdown.

**Spec:** `docs/superpowers/specs/2026-10-11-friction-log-design.md`

## Global Constraints

- The phone app keeps **no `INTERNET` permission** (CROSS-01). Don't add network code or dependencies.
- nocturned must not read gap files. `run_glob` in `src/nocturned/ingest.c` stays unchanged.
- **Byte shape.** One JSON object per line, LF only, the field order `v, ts, text, screen, track, app`, and `track` emitted as JSON `null` when absent.
- **Text.** Trimmed, newlines (CR/LF) collapsed to one space, 1–1000 characters (`GapsWriter.MAX_CHARS = 1000`).
- **`screen` values.** A Navigation route pattern (for example `now-playing` or `album/{albumId}`), or `"launcher"`, or `"desktop"`.
- **File names.** The phone writes `gaps-phone-<deviceid>.jsonl`, and the desktop script writes `gaps-desktop.jsonl`. Both live at the top level of the meta folder.
- The meta folder is on **hearth** at `/home/lucas/sync/nocturne/meta`. The phone sees it as `/storage/emulated/0/nocturne/meta`.
- **User-facing copy:**
  - Toasts: `Gap logged` and `Gap not saved — sync folder unavailable`.
  - Button labels: `+gap` and `Send`.
  - Shortcut label: `Log a gap`.
- The mini player's long-press must **not** be used.
- **App version.** Bump `versionCode` from 54 to 55 and `versionName` from `"0.4.48-dev"` to `"0.4.49-dev"`. `v0.4.48-dev` is already tagged.
- **Commits** end with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`. They never include a "Generated with Claude Code" line.
- **Build and test commands:**
  - Phone tests run from `phone/` with `./gradlew :app:testDebugUnitTest`.
  - Shell and C tests run from the repo root with `make test-e2e-watch` and `bash tests/test_jsonl_goldens.sh`. The goldens test needs `make` to have built `build/nocturned`.

---

### Task 1: GapEvent, GapsWriter and the golden

**Files:**
- Modify: `phone/app/src/main/kotlin/io/nocturne/phone/data/stats/JsonlEvents.kt` (append the class)
- Create: `phone/app/src/main/kotlin/io/nocturne/phone/data/stats/GapsWriter.kt`
- Modify: `phone/app/src/main/kotlin/io/nocturne/phone/data/AppContainer.kt` (after `actionsWriter`, around line 86)
- Create: `tests/fixtures/jsonl-goldens/gaps-golden.jsonl`
- Create: `phone/app/src/test/resources/jsonl-goldens/gaps-golden.jsonl` (an identical copy; the existing goldens are copies too, not symlinks)
- Modify: `phone/app/src/test/kotlin/io/nocturne/phone/data/stats/ByteShapeGoldenTest.kt`
- Create: `phone/app/src/test/kotlin/io/nocturne/phone/data/stats/GapsWriterTest.kt`

**Interfaces:**
- Produces:
  - `data class GapEvent(v: Int, ts: Long, text: String, screen: String, track: String?, app: String)`
  - `class GapsWriter(syncPrefs: SyncPrefs, fileWriter: JsonlFileWriter)`
    - `suspend fun emit(text: String, screen: String, track: String?): Boolean`
    - `companion object { const val MAX_CHARS = 1000; fun normalize(text: String): String; fun buildEvent(text: String, screen: String, track: String?, app: String, ts: Long): GapEvent }`
  - `AppContainer.gapsWriter: GapsWriter`

- [ ] **Step 1: Write the golden (both copies, byte-identical, LF line endings, trailing LF)**

`tests/fixtures/jsonl-goldens/gaps-golden.jsonl` and `phone/app/src/test/resources/jsonl-goldens/gaps-golden.jsonl`:

```
{"v":1,"ts":1745679800000,"text":"queue can't reorder by drag","screen":"now-playing","track":"9a3f4e1b2c5d6e7f8091a2b3c4d5e6f70819f0e1d2c3b4a5968778695a4b3c2d","app":"0.4.49-dev"}
{"v":1,"ts":1745679900000,"text":"want a sleep timer","screen":"launcher","track":null,"app":"0.4.49-dev"}
```

Check: `cmp tests/fixtures/jsonl-goldens/gaps-golden.jsonl phone/app/src/test/resources/jsonl-goldens/gaps-golden.jsonl && tail -c1 tests/fixtures/jsonl-goldens/gaps-golden.jsonl | xxd -p` should print `0a`.

- [ ] **Step 2: Add the failing byte-shape tests**

Append to the `ByteShapeGoldenTest` class, before its closing `}`:

```kotlin
    @Test fun gapsGoldenLine1_withTrack() {
        val expected = loadGolden("gaps-golden.jsonl")[0]
        val event = GapEvent(
            v = 1,
            ts = 1745679800000L,
            text = "queue can't reorder by drag",
            screen = "now-playing",
            track = "9a3f4e1b2c5d6e7f8091a2b3c4d5e6f70819f0e1d2c3b4a5968778695a4b3c2d",
            app = "0.4.49-dev",
        )
        assertEquals(expected, json.encodeToString(GapEvent.serializer(), event))
    }

    @Test fun gapsGoldenLine2_nullTrack() {
        val expected = loadGolden("gaps-golden.jsonl")[1]
        val event = GapEvent(
            v = 1,
            ts = 1745679900000L,
            text = "want a sleep timer",
            screen = "launcher",
            track = null,
            app = "0.4.49-dev",
        )
        assertEquals(expected, json.encodeToString(GapEvent.serializer(), event))
    }
```

- [ ] **Step 3: Write the failing GapsWriter tests**

`phone/app/src/test/kotlin/io/nocturne/phone/data/stats/GapsWriterTest.kt`:

```kotlin
package io.nocturne.phone.data.stats

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

/** Pure JVM: event building is a companion function so it needs no SAF. */
class GapsWriterTest {

    private val sha = "9a3f4e1b2c5d6e7f8091a2b3c4d5e6f70819f0e1d2c3b4a5968778695a4b3c2d"

    @Test fun normalize_collapsesNewlinesAndTrims() {
        assertEquals("a b c", GapsWriter.normalize("  a\nb\r\n\nc  "))
    }

    @Test fun buildEvent_carriesContext() {
        val e = GapsWriter.buildEvent("queue  ", "now-playing", sha, "0.4.49-dev", 1745679800000L)
        assertEquals(GapEvent(1, 1745679800000L, "queue", "now-playing", sha, "0.4.49-dev"), e)
    }

    @Test fun buildEvent_dropsTrackThatIsNotASha() {
        val e = GapsWriter.buildEvent("x", "now-playing", "content://not-a-sha", "0.4.49-dev", 1L)
        assertNull(e.track)
    }

    @Test(expected = IllegalArgumentException::class)
    fun buildEvent_rejectsBlank() {
        GapsWriter.buildEvent(" \n ", "now-playing", null, "0.4.49-dev", 1L)
    }

    @Test(expected = IllegalArgumentException::class)
    fun buildEvent_rejectsOverCap() {
        GapsWriter.buildEvent("x".repeat(GapsWriter.MAX_CHARS + 1), "now-playing", null, "0.4.49-dev", 1L)
    }

    @Test fun buildEvent_acceptsExactlyCap() {
        val e = GapsWriter.buildEvent("x".repeat(GapsWriter.MAX_CHARS), "now-playing", null, "0.4.49-dev", 1L)
        assertEquals(GapsWriter.MAX_CHARS, e.text.length)
    }
}
```

- [ ] **Step 4: Run the tests and confirm they fail to compile**

Run: `cd phone && ./gradlew :app:testDebugUnitTest --tests '*GapsWriterTest' --tests '*ByteShapeGoldenTest'`
Expected: compilation fails with `Unresolved reference: GapEvent` and `GapsWriter`.

- [ ] **Step 5: Add `GapEvent`**

Append to `JsonlEvents.kt`:

```kotlin
/**
 * A friction-log entry: something missing or annoying, typed by the user.
 * Written by GapsWriter to `gaps-phone-<deviceid>.jsonl`; the daemon never
 * reads it — the PM review agent does. Field order frozen; `track` is
 * emitted as JSON null when nothing was playing (Json's explicitNulls).
 */
@Serializable
data class GapEvent(
    val v: Int,
    val ts: Long,
    val text: String,                                  // 1..1000 chars, single line
    val screen: String,                                // route pattern, "launcher" or "desktop"
    val track: String?,                                // 64-char lowercase hex sha256, or null
    val app: String,                                   // BuildConfig.VERSION_NAME
)
```

- [ ] **Step 6: Add `GapsWriter`**

`phone/app/src/main/kotlin/io/nocturne/phone/data/stats/GapsWriter.kt`:

```kotlin
package io.nocturne.phone.data.stats

import androidx.core.net.toUri
import io.nocturne.phone.BuildConfig
import io.nocturne.phone.data.prefs.SyncPrefs
import kotlinx.coroutines.flow.first

/**
 * Friction-log emitter. Direct passthrough like ActionsWriter: no Room table,
 * no retry queue — a failed write returns false and the gap sheet keeps the
 * user's text so nothing typed is lost.
 *
 * Lines go to `gaps-phone-<deviceid>.jsonl` at the top level of metaDir. The
 * daemon's ingest globs never match it; the PM agent reads it over ssh.
 */
class GapsWriter(
    private val syncPrefs: SyncPrefs,
    private val fileWriter: JsonlFileWriter,
) {
    suspend fun emit(text: String, screen: String, track: String?): Boolean {
        val treeUriStr = syncPrefs.metaTreeUri.first() ?: return false
        val event = buildEvent(text, screen, track, BuildConfig.VERSION_NAME, System.currentTimeMillis())
        return fileWriter.appendLine(
            treeUri = treeUriStr.toUri(),
            relativePath = "gaps-phone-${syncPrefs.deviceId()}.jsonl",
            event = event,
            serializer = GapEvent.serializer(),
        )
    }

    companion object {
        /** 1000 UTF-16 units is at most ~3000 UTF-8 bytes: under PIPE_BUF with the other fields. */
        const val MAX_CHARS = 1000

        private val SHA256 = Regex("^[0-9a-f]{64}$")
        private val NEWLINES = Regex("[\\r\\n]+")

        fun normalize(text: String): String = text.replace(NEWLINES, " ").trim()

        fun buildEvent(text: String, screen: String, track: String?, app: String, ts: Long): GapEvent {
            val clean = normalize(text)
            require(clean.isNotEmpty()) { "gap text is empty" }
            require(clean.length <= MAX_CHARS) { "gap text over $MAX_CHARS chars: ${clean.length}" }
            return GapEvent(
                v = 1,
                ts = ts,
                text = clean,
                screen = screen,
                // Media ids are track shas today; anything else is dropped rather
                // than shipped as a malformed `track`.
                track = track?.takeIf { SHA256.matches(it) },
                app = app,
            )
        }
    }
}
```

- [ ] **Step 7: Wire it into `AppContainer`**

Insert right after the `actionsWriter` block in `AppContainer.kt`:

```kotlin
    // Friction log: one line per gap the user types. Never ingested by the
    // daemon — read by the PM review agent (docs/gaps-triage.md).
    val gapsWriter: GapsWriter by lazy { GapsWriter(syncPrefs, jsonlFileWriter) }
```

Add `import io.nocturne.phone.data.stats.GapsWriter` beside the existing `ActionsWriter` import.

- [ ] **Step 8: Run the tests and confirm they pass**

Run: `cd phone && ./gradlew :app:testDebugUnitTest --tests '*GapsWriterTest' --tests '*ByteShapeGoldenTest'`
Expected: PASS. ByteShapeGoldenTest has 11 tests and GapsWriterTest has 6.

- [ ] **Step 9: Commit**

```bash
git add tests/fixtures/jsonl-goldens/gaps-golden.jsonl phone/app/src/test/resources/jsonl-goldens/gaps-golden.jsonl \
  phone/app/src/main/kotlin/io/nocturne/phone/data/stats/JsonlEvents.kt \
  phone/app/src/main/kotlin/io/nocturne/phone/data/stats/GapsWriter.kt \
  phone/app/src/main/kotlin/io/nocturne/phone/data/AppContainer.kt \
  phone/app/src/test/kotlin/io/nocturne/phone/data/stats/ByteShapeGoldenTest.kt \
  phone/app/src/test/kotlin/io/nocturne/phone/data/stats/GapsWriterTest.kt
git commit -m "feat(phone): GapsWriter and the gaps JSONL stream

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 2: Prove the daemon side stays inert, and document the stream

**Files:**
- Modify: `tests/test_jsonl_goldens.sh` (step 1 layout)
- Modify: `tests/test_pin_cycle_runner.sh` (add case 6 before the final `if [ "$fail" -ne 0 ]`)
- Modify: `docs/jsonl-spec.md` (§2 scope list and a new §4.4)

**Interfaces:**
- Consumes: `tests/fixtures/jsonl-goldens/gaps-golden.jsonl` from Task 1.

- [ ] **Step 1: Make the goldens test carry gap files**

In `tests/test_jsonl_goldens.sh`, after the three `cp` lines under `# 1. Lay out the goldens.`, add:

```bash
# Gap files sit in the same folder and MUST be ignored: the assertions below
# (files=3, parse_errors=0, unchanged counts) are what prove it.
cp tests/fixtures/jsonl-goldens/gaps-golden.jsonl "$META/gaps-phone-G0LD.jsonl"
cp tests/fixtures/jsonl-goldens/gaps-golden.jsonl "$META/gaps-desktop.jsonl"
```

- [ ] **Step 2: Run it and confirm it passes unchanged**

Run: `make && bash tests/test_jsonl_goldens.sh`
Expected: `test_jsonl_goldens: OK`. If `files=3` fails, then ingest is reading gap files, which breaks a Global Constraint. Stop and report it; don't change the assertion.

- [ ] **Step 3: Add pin-runner case 6**

The pin-cycle `.path` unit fires on any change in the meta folder. A gap arriving must not run a cycle. Insert this before the final `if [ "$fail" -ne 0 ]` in `tests/test_pin_cycle_runner.sh`:

```bash
echo "==> case 6: only a gap arrived after the manifest -> gate stays shut"
rm -rf "$META"; mkdir -p "$META"
echo '{}' > "$META/pins-phone-test.jsonl"
sleep 0.05
: > "$META/manifest.json"
sleep 0.05
echo '{}' > "$META/gaps-phone-test.jsonl"
rm -f "$ARGS_FILE"
env HOME="$WORK/home" NOCTURNE_META_DIR="$META" \
    NOCTURNE_MANIFEST="$META/manifest.json" NOCTURNE_CYCLE_RUN="$STUB" \
    bash "$RUNNER" >"$WORK/out" 2>&1 || { echo "FAIL [gap]: non-zero exit"; fail=1; }
if [ -f "$ARGS_FILE" ]; then
    echo "FAIL [gap]: a gap triggered a cycle"; fail=1
else
    echo "    ok: gap ignored"
fi
```

- [ ] **Step 4: Run it**

Run: `bash tests/test_pin_cycle_runner.sh`
Expected: case 6 prints `ok: gap ignored`, and the run ends with `==> PASS`. The runner globs only `pins-phone-*` and `actions-phone-*`, so this passes without any runner change.

- [ ] **Step 5: Document the stream**

In `docs/jsonl-spec.md` §2, add a bullet after `pins-phone-<deviceid>.jsonl`:

```markdown
- `gaps-phone-<deviceid>.jsonl` / `gaps-desktop.jsonl` (friction log, §4.4 — not ingested)
```

After §4.3 and before `---`, add:

````markdown
### 4.4 Gap events (`gaps-phone-<deviceid>.jsonl`, `gaps-desktop.jsonl`)

Friction-log entries: something the user found missing or annoying. **The
ingester does not read these files** — `run_glob` matches only the patterns
named in §2's other bullets — and the pin-cycle runner ignores them. They are
read by the PM review agent (`.claude/agents/nocturne-pm.md`).
`tests/test_jsonl_goldens.sh` and case 6 of `tests/test_pin_cycle_runner.sh`
hold this.

```json
{"v":1,"ts":1745679800000,"text":"queue can't reorder by drag","screen":"now-playing","track":"<sha256>","app":"0.4.49-dev"}
{"v":1,"ts":1745679900000,"text":"want a sleep timer","screen":"launcher","track":null,"app":"0.4.49-dev"}
```

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `v` | integer | yes | `1`. |
| `ts` | integer | yes | Unix milliseconds (§5). |
| `text` | string | yes | 1–1000 chars, trimmed, CR/LF collapsed to a space. |
| `screen` | string | yes | Navigation route pattern (`now-playing`, `album/{albumId}`, …), `"launcher"` (opened from the app shortcut) or `"desktop"`. |
| `track` | string \| null | yes | sha256 of what was playing, or `null`. |
| `app` | string | yes | Phone version name, or `"desktop"`. |

Writers: the phone writes `gaps-phone-<deviceid>.jsonl`;
`config/bin/nocturne-gap` writes `gaps-desktop.jsonl`. One writer per file (§3).
````

- [ ] **Step 6: Commit**

```bash
git add tests/test_jsonl_goldens.sh tests/test_pin_cycle_runner.sh docs/jsonl-spec.md
git commit -m "test: gap files stay inert to ingest and the pin-cycle gate

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 3: The gap sheet and the `+gap` button on Now Playing

**Files:**
- Create: `phone/app/src/main/kotlin/io/nocturne/phone/ui/gap/GapForm.kt`
- Create: `phone/app/src/main/kotlin/io/nocturne/phone/ui/gap/GapSheet.kt`
- Modify: `phone/app/src/main/kotlin/io/nocturne/phone/ui/player/NowPlayingScreen.kt:82-86` (signature) and `:190-193` (top row)
- Modify: `phone/app/src/main/kotlin/io/nocturne/phone/ui/browser/BrowserRoot.kt` (state near line 118, the NowPlaying call near line 393, the sheet host after the `miniSheetTrack?.let { … }` block near line 438)
- Test: `phone/app/src/test/kotlin/io/nocturne/phone/ui/gap/GapFormTest.kt`

**Interfaces:**
- Consumes: `AppContainer.gapsWriter.emit(text, screen, track): Boolean` and `GapsWriter.MAX_CHARS` / `GapsWriter.normalize` from Task 1.
- Produces:
  - `class GapForm(send: suspend (String) -> Boolean)` with `val text: String`, `val canSend: Boolean`, `fun edit(new: String)` and `suspend fun submit(): Boolean`
  - `@Composable fun GapSheet(send: suspend (String) -> Boolean, onDismiss: () -> Unit)`
  - `NowPlayingScreen(controller, playerVm, onBack, onLogGap: () -> Unit)`
  - In `BrowserRoot`: `var gapScreen: String?` (null = sheet closed), which Task 4 also sets.

- [ ] **Step 1: Write the failing GapForm tests**

`phone/app/src/test/kotlin/io/nocturne/phone/ui/gap/GapFormTest.kt`:

```kotlin
package io.nocturne.phone.ui.gap

import io.nocturne.phone.data.stats.GapsWriter
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class GapFormTest {

    @Test fun cannotSendBlank() {
        val f = GapForm { true }
        f.edit("  \n ")
        assertFalse(f.canSend)
    }

    @Test fun editBeyondCapIsRefused() {
        val f = GapForm { true }
        f.edit("x".repeat(GapsWriter.MAX_CHARS))
        f.edit("x".repeat(GapsWriter.MAX_CHARS + 1))
        assertEquals(GapsWriter.MAX_CHARS, f.text.length)
    }

    @Test fun successClearsText() = runTest {
        var sent: String? = null
        val f = GapForm { sent = it; true }
        f.edit("want a sleep timer")
        assertTrue(f.submit())
        assertEquals("want a sleep timer", sent)
        assertEquals("", f.text)
    }

    @Test fun failureKeepsText() = runTest {
        val f = GapForm { false }
        f.edit("want a sleep timer")
        assertFalse(f.submit())
        assertEquals("want a sleep timer", f.text)
    }

    @Test fun submitBlankDoesNotCallSend() = runTest {
        var called = false
        val f = GapForm { called = true; true }
        assertFalse(f.submit())
        assertFalse(called)
    }
}
```

- [ ] **Step 2: Run them and confirm they fail**

Run: `cd phone && ./gradlew :app:testDebugUnitTest --tests '*GapFormTest'`
Expected: compilation fails with `Unresolved reference: GapForm`.

- [ ] **Step 3: Implement `GapForm`**

`phone/app/src/main/kotlin/io/nocturne/phone/ui/gap/GapForm.kt`:

```kotlin
package io.nocturne.phone.ui.gap

import io.nocturne.phone.data.stats.GapsWriter

/**
 * The gap sheet's logic, kept free of Compose so it unit-tests on the JVM.
 * Text survives a failed send; only a successful one clears it.
 */
class GapForm(private val send: suspend (String) -> Boolean) {
    var text: String = ""
        private set

    val canSend: Boolean get() = GapsWriter.normalize(text).isNotEmpty()

    /** Edits that would pass the cap are refused, so the field stops accepting input. */
    fun edit(new: String) {
        if (new.length <= GapsWriter.MAX_CHARS) text = new
    }

    suspend fun submit(): Boolean {
        if (!canSend) return false
        val ok = send(text)
        if (ok) text = ""
        return ok
    }
}
```

- [ ] **Step 4: Run the tests and confirm they pass**

Run: `cd phone && ./gradlew :app:testDebugUnitTest --tests '*GapFormTest'`
Expected: PASS (5 tests).

- [ ] **Step 5: Implement `GapSheet`**

`phone/app/src/main/kotlin/io/nocturne/phone/ui/gap/GapSheet.kt`:

```kotlin
package io.nocturne.phone.ui.gap

import android.widget.Toast
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.ModalBottomSheet
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import io.nocturne.phone.data.stats.GapsWriter
import io.nocturne.phone.ui.theme.JetBrainsMono
import kotlinx.coroutines.launch

/**
 * One-line friction log. `send` is GapsWriter.emit with screen + track bound
 * by the host. Back / swipe-down dismisses without writing.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun GapSheet(send: suspend (String) -> Boolean, onDismiss: () -> Unit) {
    val form = remember { GapForm(send) }
    var text by remember { mutableStateOf("") }
    var sending by remember { mutableStateOf(false) }
    val scope = rememberCoroutineScope()
    val ctx = LocalContext.current
    val focus = remember { FocusRequester() }

    ModalBottomSheet(onDismissRequest = onDismiss) {
        Column(modifier = Modifier.padding(start = 16.dp, end = 16.dp, bottom = 24.dp)) {
            Text(
                text = "~/log-gap",
                style = MaterialTheme.typography.labelSmall.copy(fontFamily = JetBrainsMono),
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            OutlinedTextField(
                value = text,
                onValueChange = { form.edit(it); text = form.text },
                placeholder = { Text("What's missing or annoying?") },
                supportingText = { Text("${text.length}/${GapsWriter.MAX_CHARS}") },
                modifier = Modifier.fillMaxWidth().padding(top = 8.dp).focusRequester(focus),
            )
            Row(modifier = Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.End) {
                TextButton(
                    enabled = form.canSend && !sending,
                    onClick = {
                        sending = true
                        scope.launch {
                            val ok = form.submit()
                            sending = false
                            if (ok) {
                                Toast.makeText(ctx, "Gap logged", Toast.LENGTH_SHORT).show()
                                onDismiss()
                            } else {
                                Toast.makeText(ctx, "Gap not saved — sync folder unavailable", Toast.LENGTH_LONG).show()
                            }
                        }
                    },
                ) { Text("Send") }
            }
        }
    }
    LaunchedEffect(Unit) { focus.requestFocus() }
}
```

- [ ] **Step 6: Give `NowPlayingScreen` a `+gap` button**

Change the signature at `NowPlayingScreen.kt:82-86` to:

```kotlin
fun NowPlayingScreen(
    controller: MediaController,
    playerVm: PlayerViewModel,
    onBack: () -> Unit,
    onLogGap: () -> Unit,
) {
```

In the top row, replace the `Spacer(Modifier.weight(1f))` line and the three-line "Heart icon intentionally absent" comment after it (around lines 190-193) with:

```kotlin
                Spacer(Modifier.weight(1f))
                // Friction log. The heart's old slot; the mini player's
                // long-press is reserved for hold-to-unpin.
                Text(
                    text = "+gap",
                    style = MaterialTheme.typography.labelSmall.copy(fontFamily = JetBrainsMono),
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier
                        .clickable(onClick = onLogGap)
                        .padding(start = 24.dp, top = 8.dp, bottom = 8.dp),
                )
                // Heart icon intentionally absent. If a like-toggle is added
                // later, re-introduce LikeDao.isLiked(...) Flow + a VM
                // toggle method — both were deleted as dead code.
```

- [ ] **Step 7: Host the sheet in `BrowserRoot`**

Next to `var miniSheetTrack by remember { … }` (around line 118), add:

```kotlin
    // Friction log: non-null = sheet open, holding the `screen` value to log.
    var gapScreen by rememberSaveable { mutableStateOf<String?>(null) }
```

Add `import androidx.compose.runtime.saveable.rememberSaveable` if it isn't already imported. Also add `import io.nocturne.phone.ui.gap.GapSheet`.

At the `NowPlayingScreen(` call (around line 393), add the new argument:

```kotlin
                        NowPlayingScreen(
                            controller = activeController,
                            playerVm = playerVm,
                            onBack = { nav.popBackStack() },
                            onLogGap = { gapScreen = Routes.NOW_PLAYING },
                        )
```

Right after the `miniSheetTrack?.let { t -> TrackActionsSheet(…) }` block (the last child inside the outer `Box`, around line 438), add:

```kotlin
        gapScreen?.let { screen ->
            GapSheet(
                send = { text ->
                    container.gapsWriter.emit(
                        text = text,
                        screen = screen,
                        track = playerVm.controller.value?.currentMediaItem?.mediaId,
                    )
                },
                onDismiss = { gapScreen = null },
            )
        }
```

- [ ] **Step 8: Build and run the full unit suite**

Run: `cd phone && ./gradlew :app:testDebugUnitTest :app:assembleDebug`
Expected: BUILD SUCCESSFUL, with all tests passing.

- [ ] **Step 9: Commit**

```bash
git add phone/app/src/main/kotlin/io/nocturne/phone/ui/gap \
  phone/app/src/test/kotlin/io/nocturne/phone/ui/gap \
  phone/app/src/main/kotlin/io/nocturne/phone/ui/player/NowPlayingScreen.kt \
  phone/app/src/main/kotlin/io/nocturne/phone/ui/browser/BrowserRoot.kt
git commit -m "feat(phone): gap sheet, opened from +gap on Now Playing

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 4: The "Log a gap" launcher shortcut and the version bump

**Files:**
- Create: `phone/app/src/main/res/xml/shortcuts.xml`
- Modify: `phone/app/src/main/res/values/strings.xml`
- Modify: `phone/app/src/main/AndroidManifest.xml:29-37` (the MainActivity element)
- Modify: `phone/app/src/main/kotlin/io/nocturne/phone/MainActivity.kt`
- Modify: `phone/app/src/main/kotlin/io/nocturne/phone/data/AppContainer.kt`
- Modify: `phone/app/src/main/kotlin/io/nocturne/phone/ui/browser/BrowserRoot.kt`
- Modify: `phone/app/build.gradle.kts:20-21`

**Interfaces:**
- Consumes: `var gapScreen` in `BrowserRoot` (Task 3).
- Produces:
  - `AppContainer.gapRequests: MutableStateFlow<Boolean>`
  - `MainActivity.ACTION_LOG_GAP = "io.nocturne.phone.action.LOG_GAP"`

- [ ] **Step 1: Add the shortcut resource**

`phone/app/src/main/res/xml/shortcuts.xml`:

```xml
<?xml version="1.0" encoding="utf-8"?>
<shortcuts xmlns:android="http://schemas.android.com/apk/res/android">
    <shortcut
        android:shortcutId="log_gap"
        android:enabled="true"
        android:icon="@mipmap/ic_launcher"
        android:shortcutShortLabel="@string/shortcut_log_gap">
        <intent
            android:action="io.nocturne.phone.action.LOG_GAP"
            android:targetPackage="io.nocturne.phone"
            android:targetClass="io.nocturne.phone.MainActivity" />
    </shortcut>
</shortcuts>
```

Add this inside `<resources>` in `strings.xml`:

```xml
    <string name="shortcut_log_gap">Log a gap</string>
```

In `AndroidManifest.xml`, inside the MainActivity `<activity>` and after its `</intent-filter>`, add:

```xml
            <meta-data
                android:name="android.app.shortcuts"
                android:resource="@xml/shortcuts" />
```

- [ ] **Step 2: Add the request flag to `AppContainer`**

Next to `gapsWriter`:

```kotlin
    // Set by MainActivity when launched from the "Log a gap" shortcut;
    // BrowserRoot opens the sheet and clears it.
    val gapRequests = kotlinx.coroutines.flow.MutableStateFlow(false)
```

- [ ] **Step 3: Handle the intent in `MainActivity`**

Replace the class body:

```kotlin
class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        WindowCompat.setDecorFitsSystemWindows(window, false)
        val app = application as NocturneApp
        // savedInstanceState != null means a config-change recreate re-delivering
        // the same shortcut intent — the sheet was already opened once.
        if (savedInstanceState == null) handleGapIntent(intent)
        setContent { NocturneTheme { AppRoot(app) } }
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        handleGapIntent(intent)
    }

    private fun handleGapIntent(intent: Intent?) {
        if (intent?.action == ACTION_LOG_GAP) {
            (application as NocturneApp).container.gapRequests.value = true
        }
    }

    companion object {
        const val ACTION_LOG_GAP = "io.nocturne.phone.action.LOG_GAP"
    }
}
```

Add `import android.content.Intent`.

- [ ] **Step 4: Open the sheet from `BrowserRoot`**

Directly after the `var gapScreen by rememberSaveable { … }` line from Task 3, add:

```kotlin
    val gapRequested by container.gapRequests.collectAsStateWithLifecycle()
    LaunchedEffect(gapRequested) {
        if (gapRequested) {
            gapScreen = "launcher"
            container.gapRequests.value = false
        }
    }
```

`collectAsStateWithLifecycle` and `LaunchedEffect` are already used in this file. Add any missing import (`androidx.compose.runtime.LaunchedEffect`, `androidx.lifecycle.compose.collectAsStateWithLifecycle`).

- [ ] **Step 5: Bump the version**

In `phone/app/build.gradle.kts`: `versionCode = 55` and `versionName = "0.4.49-dev"`.

- [ ] **Step 6: Build, run the tests, and check the manifest merged the shortcut**

Run: `cd phone && ./gradlew :app:testDebugUnitTest :app:assembleDebug && ~/Android/Sdk/build-tools/36.0.0/aapt2 dump xmltree app/build/outputs/apk/debug/app-debug.apk --file AndroidManifest.xml | grep -A2 'android.app.shortcuts'`
Expected: BUILD SUCCESSFUL, and the grep shows the `meta-data` that points at the shortcuts resource.

- [ ] **Step 7: Commit**

```bash
git add phone/app/src/main/res/xml/shortcuts.xml phone/app/src/main/res/values/strings.xml \
  phone/app/src/main/AndroidManifest.xml phone/app/src/main/kotlin/io/nocturne/phone/MainActivity.kt \
  phone/app/src/main/kotlin/io/nocturne/phone/data/AppContainer.kt \
  phone/app/src/main/kotlin/io/nocturne/phone/ui/browser/BrowserRoot.kt phone/app/build.gradle.kts
git commit -m "feat(phone): \"Log a gap\" launcher shortcut; bump to 0.4.49-dev

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 5: `nocturne-gap` desktop script

**Files:**
- Create: `config/bin/nocturne-gap` (mode 755)
- Create: `tests/test_nocturne_gap.sh`
- Modify: `Makefile` (the `test-e2e-watch` recipe, after the `test_pin_cycle_runner.sh` lines)

**Interfaces:**
- Produces:
  - `nocturne-gap TEXT...` appends one line to `gaps-desktop.jsonl`
  - `nocturne-gap -l` prints `<ts>  <YYYY-MM-DD HH:MM>  <screen>  <text>` for every gap, oldest first
  - Env vars: `NOCTURNE_GAP_HOST` (default `hearth`; empty means local) and `NOCTURNE_GAP_META` (default `/home/lucas/sync/nocturne/meta`)

- [ ] **Step 1: Write the failing test**

`tests/test_nocturne_gap.sh`:

```bash
#!/usr/bin/env bash
# Unit test for config/bin/nocturne-gap: append + list, local mode.
#
# What it proves:
#   1. Two appends produce two valid JSONL lines with the frozen field order.
#   2. Newlines in the text are collapsed; surrounding space is trimmed.
#   3. -l merges desktop + phone files, oldest first, one line per gap.
#   4. Empty text and over-1000-char text are refused (exit 2), file untouched.
#   5. A trailing partial line (mid-Syncthing) does not break -l.
#
# Hermetic: NOCTURNE_GAP_HOST= runs everything locally against a temp meta dir.

set -euo pipefail

GAP="${GAP:-config/bin/nocturne-gap}"
test -f "$GAP" || { echo "missing: $GAP" >&2; exit 1; }

WORK=$(mktemp -d -t nocturne-gap-XXXXXX)
trap 'rm -rf "$WORK"' EXIT
export NOCTURNE_GAP_HOST= NOCTURNE_GAP_META="$WORK"
fail=0

sh "$GAP" "want a sleep timer"
sh "$GAP" "  queue
can't reorder  "
F="$WORK/gaps-desktop.jsonl"

echo "==> case 1: two valid lines, frozen field order"
[ "$(wc -l < "$F")" -eq 2 ] || { echo "FAIL: expected 2 lines"; fail=1; }
keys=$(head -1 "$F" | jq -c 'keys_unsorted')
[ "$keys" = '["v","ts","text","screen","track","app"]' ] || { echo "FAIL: field order $keys"; fail=1; }
head -1 "$F" | jq -e '.v==1 and .screen=="desktop" and .track==null and .app=="desktop"' >/dev/null \
    || { echo "FAIL: fixed fields"; fail=1; }

echo "==> case 2: newlines collapsed, trimmed"
[ "$(sed -n 2p "$F" | jq -r .text)" = "queue can't reorder" ] || { echo "FAIL: got '$(sed -n 2p "$F" | jq -r .text)'"; fail=1; }

echo "==> case 3: -l merges files, oldest first"
printf '%s\n' '{"v":1,"ts":1577836800001,"text":"phone gap","screen":"now-playing","track":null,"app":"0.4.49-dev"}' \
    > "$WORK/gaps-phone-abcd1234.jsonl"
out=$(sh "$GAP" -l)
[ "$(printf '%s\n' "$out" | wc -l)" -eq 3 ] || { echo "FAIL: -l printed:"; echo "$out"; fail=1; }
printf '%s\n' "$out" | head -1 | grep -q 'now-playing  phone gap$' || { echo "FAIL: oldest not first"; echo "$out"; fail=1; }

echo "==> case 4: refusals"
before=$(wc -l < "$F")
if sh "$GAP" "   " 2>/dev/null; then echo "FAIL: blank accepted"; fail=1; fi
if sh "$GAP" "$(printf 'x%.0s' $(seq 1001))" 2>/dev/null; then echo "FAIL: 1001 chars accepted"; fail=1; fi
[ "$(wc -l < "$F")" -eq "$before" ] || { echo "FAIL: refused text was written"; fail=1; }

echo "==> case 5: trailing partial line tolerated"
printf '%s' '{"v":1,"ts":15778' >> "$WORK/gaps-phone-abcd1234.jsonl"
sh "$GAP" -l >/dev/null || { echo "FAIL: -l died on a partial line"; fail=1; }

[ "$fail" -eq 0 ] || { echo "==> FAILED"; exit 1; }
echo "==> PASS"
```

- [ ] **Step 2: Run it and confirm it fails**

Run: `bash tests/test_nocturne_gap.sh`
Expected: `missing: config/bin/nocturne-gap` and exit 1.

- [ ] **Step 3: Write the script**

`config/bin/nocturne-gap`:

```sh
#!/bin/sh
# nocturne-gap -- the friction log: note something nocturne is missing.
#
#   nocturne-gap queue view should show album art   append a gap
#   nocturne-gap -l                                  list every gap, oldest first
#
# Gaps live beside the phone's in the Syncthing meta folder on hearth:
# the phone writes gaps-phone-<deviceid>.jsonl, this writes gaps-desktop.jsonl
# (one writer per file, docs/jsonl-spec.md §3, §4.4). nocturned never reads
# them; the PM agent (.claude/agents/nocturne-pm.md) does.
#
#   NOCTURNE_GAP_HOST   ssh host holding the meta folder (default hearth;
#                       empty = this machine)
#   NOCTURNE_GAP_META   meta folder path on that host
set -eu

HOST="${NOCTURNE_GAP_HOST-hearth}"
META="${NOCTURNE_GAP_META:-/home/lucas/sync/nocturne/meta}"
MAX=1000

die() { printf 'nocturne-gap: %s\n' "$1" >&2; exit 2; }

# on_meta CMD -- run a shell command against the meta folder, here or on HOST.
on_meta() {
    if [ -n "$HOST" ]; then ssh "$HOST" "cd '$META' && $1"; else (cd "$META" && sh -c "$1"); fi
}

list() {
    # fromjson? skips a trailing partial line Syncthing has not finished.
    on_meta 'cat gaps-*.jsonl 2>/dev/null || true' |
        jq -Rr 'fromjson? | select(.ts) | [.ts, .screen, .text] | @tsv' |
        sort -n |
        while IFS="$(printf '\t')" read -r ts screen text; do
            printf '%s  %s  %s  %s\n' "$ts" "$(date -d "@$((ts / 1000))" '+%Y-%m-%d %H:%M')" "$screen" "$text"
        done
}

append() {
    text=$(printf '%s' "$*" | tr '\r\n' '  ' | sed 's/  */ /g; s/^ //; s/ $//')
    [ -n "$text" ] || die "empty gap"
    [ "${#text}" -le "$MAX" ] || die "gap over $MAX characters (${#text})"
    line=$(jq -cn --arg text "$text" --argjson ts "$(date +%s%3N)" \
        '{v:1, ts:$ts, text:$text, screen:"desktop", track:null, app:"desktop"}')
    # stdin carries the line, so no quoting survives into the remote shell.
    printf '%s\n' "$line" | on_meta 'cat >> gaps-desktop.jsonl'
}

case "${1-}" in
    -l|--list) list ;;
    -h|--help|'') sed -n '2,/^set -eu/p' "$0" | sed 's/^# \{0,1\}//; /^set -eu/d' ;;
    *) append "$@" ;;
esac
```

Then: `chmod 755 config/bin/nocturne-gap`

Note: `${#text}` counts characters only in a UTF-8 locale. The test uses ASCII, and the phone enforces the real cap. That's enough for a desktop helper.

- [ ] **Step 4: Run the test and confirm it passes**

Run: `bash tests/test_nocturne_gap.sh`
Expected: `==> PASS`.

- [ ] **Step 5: Add it to the Makefile**

In the `test-e2e-watch` recipe, after `@bash tests/test_pin_cycle_runner.sh`:

```make
	@echo "==> Running tests/test_nocturne_gap.sh"
	@bash tests/test_nocturne_gap.sh
```

The recipe lines use a tab, matching the lines around them.

Run: `make test-e2e-watch`
Expected: every listed test passes, including the new one.

- [ ] **Step 6: Commit**

```bash
git add config/bin/nocturne-gap tests/test_nocturne_gap.sh Makefile
git commit -m "feat: nocturne-gap, the desktop side of the friction log

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 6: PM review agent and the triage file

**Files:**
- Create: `.claude/agents/nocturne-pm.md`
- Create: `docs/gaps-triage.md`

**Interfaces:**
- Consumes: `nocturne-gap -l` (Task 5) and the raw `gaps-*.jsonl` files over `ssh hearth`.

- [ ] **Step 1: Check that `.claude/` isn't ignored**

Run: `git check-ignore -v .claude/agents/nocturne-pm.md; echo rc=$?`
Expected: `rc=1`, meaning the path isn't ignored. If it prints an ignore rule, tell the user before continuing. Don't edit `.gitignore` without asking.

- [ ] **Step 2: Write the agent**

`.claude/agents/nocturne-pm.md`:

```markdown
---
name: nocturne-pm
description: Product review for nocturne. Reads the user's friction log (gaps typed on the phone or with nocturne-gap) plus the code, and returns ranked proposals and questions. Use when the user asks to "run the nocturne PM", review gaps, or decide what to build next. Read-only except docs/gaps-triage.md.
tools: Bash, Read, Grep, Glob, Edit, Write
---

You are the product reviewer for nocturne, a single-user, self-hosted music
system (C daemon `nocturned` on hearth, Android app in `phone/`, Syncthing as
the only transport). The user is its only user. You cannot know what they
want beyond what they logged — **the gaps are the signal; do not invent
needs from generic music-app features.**

## Inputs

1. Every gap: run `nocturne-gap -l` (columns: ts, local time, screen, text).
   For the `track` and `app` fields, read the raw files:
   `ssh hearth 'cat /home/lucas/sync/nocturne/meta/gaps-*.jsonl'`.
   A gap's identity is its `ts`.
2. `docs/gaps-triage.md` — gaps already triaged. Any ts not in its table is new.
3. `README.md`, `CLAUDE.md`, `docs/` — what exists and the hard boundaries.
4. The code, read selectively — only what the gaps point at.

To name a track from a gap's sha: `ssh hearth "sqlite3 ~/.local/share/nocturne/nocturne.db \"select path from tracks where sha256='<sha>'\""`.

## Hard boundaries (a proposal that crosses one must say so)

- The phone app has no INTERNET permission (CROSS-01).
- `nocturned` makes no non-loopback network call (CROSS-03); network work
  goes in a separate binary (`flacget`, `nocturne-recommend`).
- Syncthing is the only phone↔desktop transport.

## Output (in chat)

1. **Proposals, ranked** by how much friction they remove for this user.
   Each one has: a title, the gap ts values it answers, what changes for
   the user, the files it would touch, size S/M/L, and any boundary it
   crosses. Several gaps may become one proposal.
2. **Half-built paths** — code that is most of the way to something useful
   (unused columns, dead routes, a comment describing a removed feature).
   Only list ones that relate to a logged gap.
3. **Questions** — anywhere the user's intent is ambiguous. Ask; do not pick.

Keep it short. No generic advice, no proposals without a gap behind them
(except in section 2).

## The only file you write: docs/gaps-triage.md

Add a row per new gap with status `open`, and update statuses the user
confirms in this conversation (`planned`, `done`, `declined`). Never edit
code, never commit, never touch the JSONL files.
```

- [ ] **Step 3: Write the triage file**

`docs/gaps-triage.md`:

```markdown
# Gaps triage

Friction-log entries (`nocturne-gap -l`) and what became of them. Maintained by
the `nocturne-pm` agent; the JSONL files stay append-only. A ts missing from
this table is untriaged.

Status: `open` · `planned` · `done` · `declined`

| ts | gap | status | note |
|---|---|---|---|
```

- [ ] **Step 4: Check the agent loads**

Run: `claude agents 2>/dev/null | grep -i nocturne-pm || ls .claude/agents/`
Expected: `nocturne-pm` is listed, or at minimum the file exists. Agents load at session start, so a running session needs a restart to see it.

- [ ] **Step 5: Commit**

```bash
git add .claude/agents/nocturne-pm.md docs/gaps-triage.md
git commit -m "feat: nocturne-pm review agent and gaps triage file

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 7: Rollout and manual acceptance (needs the user)

**Files:** none in the repo, apart from the pushes and the tag.

- [ ] **Step 1: Install the script on soot**

Run: `ln -sf ~/src/nocturne/config/bin/nocturne-gap ~/.local/bin/nocturne-gap && nocturne-gap -l; echo rc=$?`
Expected: `rc=0`, with no output because there are no gaps yet.

- [ ] **Step 2: Seed the log from `todo.md`**

```sh
nocturne-gap "Improve the queue view"
nocturne-gap "Hold to delete/unpin the mini player's song"
nocturne-gap -l
```

Expected: two lines, both with screen `desktop`. Both gaps came from **one** line in `~/notes/todo.md`. Delete that single line, after showing the user which line is going:
`#Improve queue view and hold to delete/unpin mini player's song on nocturne`

Leave `#Implement download new songs to library via nocturne` alone. It isn't part of this seed, so ask the user whether it's done or should become a gap too.

- [ ] **Step 3: Install a test build on the phone**

Run (from `phone/`): `./gradlew :app:assembleDebug && adb install -r --user 10 app/build/outputs/apk/debug/app-debug.apk`

If the install fails with a signature mismatch, the installed app is CI-signed. Skip this step and test after Step 5's release instead. Don't uninstall the app: that would wipe its state.

- [ ] **Step 4: Manual acceptance (the user, on the phone)**

1. Long-press the nocturne icon → **Log a gap**. The sheet opens with the keyboard up. Type `test from launcher` and tap Send. A "Gap logged" toast appears.
2. Play something, open Now Playing, tap **+gap**, type `test from now playing`, and tap Send.
3. On soot: `nocturne-gap -l`. Within about a minute of Syncthing it shows both, with screens `launcher` and `now-playing`.
4. Run the agent once ("run the nocturne PM"). It lists the 4 gaps and adds them to `docs/gaps-triage.md` as `open`.
5. Remove the two test gaps from the triage table by marking them `declined` with the note "acceptance test".

- [ ] **Step 5: Ship**

Only after the user approves:
1. Push the branch, then merge into main through the `superpowers:finishing-a-development-branch` flow.
2. Tag `v0.4.49-dev` on main and push the tag. CI builds and signs the APK, and Obtainium picks it up.
