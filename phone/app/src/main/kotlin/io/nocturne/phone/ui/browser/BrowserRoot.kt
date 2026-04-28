package io.nocturne.phone.ui.browser

import androidx.activity.compose.BackHandler
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Search
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.material3.TopAppBarDefaults
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import kotlinx.coroutines.launch
import io.nocturne.phone.ui.browser.components.NocturneBottomNav
import io.nocturne.phone.ui.player.MiniPlayer
import io.nocturne.phone.ui.player.NowPlayingScreen
import io.nocturne.phone.ui.settings.SettingsScreen
import androidx.lifecycle.viewmodel.compose.viewModel
import androidx.navigation.NavGraph.Companion.findStartDestination
import androidx.navigation.NavType
import androidx.navigation.compose.NavHost
import androidx.navigation.compose.composable
import androidx.navigation.compose.currentBackStackEntryAsState
import androidx.navigation.compose.rememberNavController
import androidx.compose.ui.text.SpanStyle
import androidx.compose.ui.text.buildAnnotatedString
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import io.nocturne.phone.ui.theme.JetBrainsMono
import androidx.compose.ui.text.withStyle
import androidx.navigation.navArgument
import io.nocturne.phone.data.AppContainer
import io.nocturne.phone.player.PlayerVMFactory
import io.nocturne.phone.player.PlayerViewModel
import io.nocturne.phone.ui.search.SearchOverlay
import io.nocturne.phone.ui.system.RotationScreen
import io.nocturne.phone.ui.system.StatsScreen
import io.nocturne.phone.ui.system.StorageScreen
import io.nocturne.phone.ui.system.SyncScreen
import io.nocturne.phone.ui.system.UtilityBar

/**
 * Top-level browser surface. Hosts the four-tab NavigationBar plus detail
 * destinations. AppRoot mounts this once it has confirmed metaTreeUri is
 * persisted AND the DB has at least one track.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun BrowserRoot(
    container: AppContainer,
    requestPlay: (() -> Unit) -> Unit,
) {
    // Quick task 260428-8i6: requestPlay is the AppRoot-hosted gate's submission
    // lambda. Tap-to-play sites below wrap their playback action inside
    // requestPlay { ... } so the FirstPlayNotifGate decides whether to show the
    // rationale (first time) or run immediately.
    val vm: BrowserViewModel = viewModel(factory = BrowserVMFactory(container))
    val playerVm: PlayerViewModel = viewModel(factory = PlayerVMFactory(container))
    DisposableEffect(playerVm) {
        playerVm.connect()
        onDispose { playerVm.disconnect() }
    }
    val nav = rememberNavController()
    var showSearch by remember { mutableStateOf(false) }
    // Quick task 260428-ja8: the System affordance is now an in-place utility
    // mode toggle (◇ → ◆) instead of a routed hub. `inUtility` flips the shell
    // to render UtilityBar + inline rotation/sync/storage/stats content; the
    // bottom nav + mini-player hide while in utility. plain `remember` (not
    // Saveable) is intentional — utility mode is an ephemeral focus state and
    // process death / rotation should reset to browse for predictability.
    var inUtility by remember { mutableStateOf(false) }
    var activeUtility by remember { mutableStateOf("rotation") }
    // System back exits utility mode rather than the app — utility is an
    // overlay state, not a navigation destination.
    BackHandler(enabled = inUtility) { inUtility = false }

    Box(modifier = Modifier.fillMaxSize()) {
        // Track the active route so we can hide the bottom-bar (nav + mini-player)
        // on the NowPlaying / detail screens that own their own bottom chrome.
        // Utility mode is hosted in-place (no route change) — its chrome is
        // gated separately via `inUtility` below.
        val currentRoute = nav.currentBackStackEntryAsState().value?.destination?.route
        val isFullscreen = currentRoute == Routes.NOW_PLAYING

        // Live mini-player visibility — drives both the inclusion of the
        // mini-row in the bottom-bar slot AND its content.
        val activeController = playerVm.controller.collectAsStateWithLifecycle().value
        var hasMediaItem by remember(activeController) {
            mutableStateOf(activeController?.currentMediaItem != null)
        }
        if (activeController != null) {
            DisposableEffect(activeController) {
                val listener = object : androidx.media3.common.Player.Listener {
                    override fun onMediaItemTransition(
                        mediaItem: androidx.media3.common.MediaItem?,
                        reason: Int,
                    ) {
                        hasMediaItem = mediaItem != null
                    }
                    override fun onTimelineChanged(
                        timeline: androidx.media3.common.Timeline,
                        reason: Int,
                    ) {
                        hasMediaItem = activeController.currentMediaItem != null
                    }
                }
                activeController.addListener(listener)
                onDispose { activeController.removeListener(listener) }
            }
        }

        Scaffold(
            topBar = {
                if (!isFullscreen) {
                    Column {
                        TopAppBar(
                            title = { BrandWordmark() },
                            actions = {
                                // Quick task 260428-ja8: ◇ toggles the shell
                                // into utility mode in place (no nav, no
                                // back-stack). Active mode swaps the glyph to
                                // ◆ + primary tint so the affordance reads
                                // engaged. Text-as-glyph (JetBrains Mono is
                                // bundled — see Typography.kt) avoids adding
                                // a material-icons-extended dependency for
                                // one monogram. Positioned BEFORE Search so
                                // the glyph sits to the left of the magnifier.
                                IconButton(onClick = { inUtility = !inUtility }) {
                                    Text(
                                        text = if (inUtility) "◆" else "◇",
                                        style = MaterialTheme.typography.titleMedium.copy(
                                            fontFamily = JetBrainsMono,
                                            fontWeight = FontWeight.Normal,
                                        ),
                                        color = if (inUtility) MaterialTheme.colorScheme.primary
                                                else MaterialTheme.colorScheme.onSurface,
                                    )
                                }
                                IconButton(onClick = { showSearch = true }) {
                                    Icon(
                                        imageVector = Icons.Default.Search,
                                        contentDescription = "Search",
                                    )
                                }
                            },
                            colors = TopAppBarDefaults.topAppBarColors(
                                containerColor = MaterialTheme.colorScheme.surface,
                                titleContentColor = MaterialTheme.colorScheme.onSurface,
                                actionIconContentColor = MaterialTheme.colorScheme.onSurface,
                            ),
                        )
                        // 1px hairline under the brand row, color #c5c0b9 per
                        // design pass2026-04-28 hand-tuning.
                        Box(
                            modifier = Modifier
                                .fillMaxWidth()
                                .height(1.dp)
                                .background(androidx.compose.ui.graphics.Color(0xFFC5C0B9)),
                        )
                        // Quick task 260428-ja8: when in utility mode, render
                        // the 4-tab UtilityBar directly below the brand row so
                        // Scaffold lays content out below it without manual
                        // offset math.
                        if (inUtility) {
                            UtilityBar(
                                active = activeUtility,
                                onChange = { activeUtility = it },
                            )
                        }
                    }
                }
            },
            bottomBar = {
                if (isFullscreen || inUtility) {
                    // NowPlaying owns its own bottom transport block — no mini,
                    // no nav. Utility mode hides both so the inline rotation/
                    // sync/storage/stats content occupies the full slot below
                    // the UtilityBar (260428-ja8).
                    return@Scaffold
                }
                // Bottom slot is mini-player (when present) stacked on top of the
                // navigation bar. Stacked in one slot so they don't overlap and
                // the mini stays anchored above the system gesture inset.
                Column(modifier = Modifier.fillMaxWidth()) {
                    if (hasMediaItem && activeController != null) {
                        MiniPlayer(
                            controller = activeController,
                            onTap = { nav.navigate(Routes.NOW_PLAYING) },
                        )
                    }
                    // 1dp top hairline #837A6C above the bottom nav. Warmer
                    // tan tone separates nav from mini-player / content above
                    // (revert from the dark #1A1A1A pass — user spec update).
                    Box(
                        modifier = Modifier
                            .fillMaxWidth()
                            .height(1.dp)
                            .background(androidx.compose.ui.graphics.Color(0xFF837A6C)),
                    )
                    // Map active route to its tab so the selected state survives
                    // navigation into detail screens (album / artist detail).
                    val activeTabRoute: String? = when {
                        currentRoute == Routes.ALBUMS -> Routes.ALBUMS
                        currentRoute == Routes.ARTISTS -> Routes.ARTISTS
                        currentRoute == Routes.TRACKS -> Routes.TRACKS
                        currentRoute == Routes.GENRES -> Routes.GENRES
                        currentRoute == Routes.ALBUM_DETAIL_PATTERN -> Routes.ALBUMS
                        currentRoute == Routes.ARTIST_DETAIL_PATTERN -> Routes.ARTISTS
                        else -> null
                    }
                    NocturneBottomNav(
                        activeRoute = activeTabRoute,
                        onTab = { route ->
                            nav.navigate(route) {
                                popUpTo(nav.graph.findStartDestination().id) {
                                    saveState = true
                                }
                                launchSingleTop = true
                                restoreState = true
                            }
                        },
                    )
                }
            },
        ) { padding ->
            if (inUtility) {
                // Quick task 260428-ja8: utility mode is hosted in-place
                // (no nav transition, no back-stack). The 4 sub-screens are
                // selected by activeUtility and render as inline content
                // composables below the UtilityBar slot owned by the topBar.
                Box(modifier = Modifier.fillMaxSize().padding(padding)) {
                    when (activeUtility) {
                        "rotation" -> RotationScreen(container = container)
                        "sync"     -> SyncScreen(container = container)
                        "storage"  -> StorageScreen(container = container)
                        "stats"    -> StatsScreen(container = container)
                    }
                }
                return@Scaffold
            }
            NavHost(
                navController = nav,
                startDestination = Routes.ALBUMS,
                modifier = Modifier.padding(padding),
            ) {
                composable(Routes.ALBUMS) {
                    AlbumsScreen(vm, onNavigate = { id -> nav.navigate(Routes.albumDetail(id)) }, container = container)
                }
                composable(Routes.ARTISTS) {
                    ArtistsScreen(vm, onNavigate = { id -> nav.navigate(Routes.artistDetail(id)) })
                }
                composable(Routes.TRACKS) {
                    val scope = androidx.compose.runtime.rememberCoroutineScope()
                    val ctx = androidx.compose.ui.platform.LocalContext.current
                    TracksScreen(
                        vm = vm,
                        onTrackTap = { track ->
                            requestPlay {
                                scope.launch {
                                    val all = vm.tracksAllList()
                                    playerVm.playFromList(all, track)
                                    nav.navigate(Routes.NOW_PLAYING)
                                }
                            }
                        },
                        onTrackLongPress = { track ->
                            playerVm.enqueueTrack(track)
                            android.widget.Toast.makeText(
                                ctx,
                                "Added to queue",
                                android.widget.Toast.LENGTH_SHORT,
                            ).show()
                        },
                    )
                }
                composable(Routes.GENRES) { GenresScreen(vm) }
                composable(Routes.SETTINGS) {
                    SettingsScreen(container = container)
                }
                composable(
                    route = Routes.ALBUM_DETAIL_PATTERN,
                    arguments = listOf(navArgument("albumId") { type = NavType.StringType }),
                ) { entry ->
                    val id = entry.arguments?.getString("albumId") ?: return@composable
                    AlbumDetailScreen(
                        albumId = id,
                        vm = vm,
                        playerVm = playerVm,
                        requestPlay = requestPlay,
                        onBack = { nav.popBackStack() },
                        onPlayStarted = { nav.navigate(Routes.NOW_PLAYING) },
                    )
                }
                composable(
                    route = Routes.ARTIST_DETAIL_PATTERN,
                    arguments = listOf(navArgument("artistId") { type = NavType.StringType }),
                ) { entry ->
                    val id = entry.arguments?.getString("artistId") ?: return@composable
                    ArtistDetailScreen(
                        artistId = id,
                        vm = vm,
                        onBack = { nav.popBackStack() },
                        onAlbumTap = { albumId -> nav.navigate(Routes.albumDetail(albumId)) },
                        onTrackTap = { track ->
                            requestPlay {
                                playerVm.playSingleTrack(track)
                                nav.navigate(Routes.NOW_PLAYING)
                            }
                        },
                        onTrackLongPress = { track -> playerVm.enqueueTrack(track) },
                        container = container,
                    )
                }
                composable(Routes.NOW_PLAYING) {
                    val activeController = playerVm.controller.collectAsStateWithLifecycle().value
                    if (activeController != null) {
                        NowPlayingScreen(
                            controller = activeController,
                            playerVm = playerVm,
                            onBack = { nav.popBackStack() },
                        )
                    } else {
                        Box(
                            modifier = Modifier
                                .fillMaxSize()
                                .background(MaterialTheme.colorScheme.background),
                        )
                    }
                }
            }
        }
        if (showSearch) {
            val pinnedIds by vm.pinnedIdSet.collectAsStateWithLifecycle()
            SearchOverlay(
                container = container,
                onDismiss = { showSearch = false },
                pinnedIds = pinnedIds,
                onPinTrack = { vm.togglePinTrack(it) },
                onTrackTap = { track ->
                    // Quick task 260428-8i6: dismiss the overlay BEFORE submitting
                    // to requestPlay so the gate's AlertDialog (if any) is not
                    // rendered behind the still-present overlay scrim.
                    showSearch = false
                    requestPlay {
                        playerVm.playSingleTrack(track)
                        nav.navigate(Routes.NOW_PLAYING)
                    }
                },
            )
        }
        // Mini-player + nav-bar are now stacked in the Scaffold bottomBar slot
        // above (no longer overlaid via Box.align so they can't visually clash
        // with the NavigationBar or the gesture inset).
    }
}

/**
 * Top-bar wordmark — `$ nocturne▌` prompt-style per the design pass2026-04-27
 * design pass (ratified default `brandMode: 'lower'`). Monospace family with the
 * cursor block in the primary accent color, leaning into the project's terminal
 * aesthetic without adding a font dependency (system mono is sufficient on
 * GrapheneOS + Pixel; F-Droid reproducible-build path stays clean).
 */
@Composable
private fun BrandWordmark() {
    val nameColor = MaterialTheme.colorScheme.onSurface
    val accent = MaterialTheme.colorScheme.primary
    val cursorColor = accent
    Text(
        text = buildAnnotatedString {
            withStyle(SpanStyle(color = accent)) { append("$ ") }
            withStyle(SpanStyle(color = nameColor)) { append("nocturne") }
            withStyle(SpanStyle(color = cursorColor)) { append(" ▌") }
        },
        style = MaterialTheme.typography.titleMedium.copy(fontFamily = JetBrainsMono),
    )
}
