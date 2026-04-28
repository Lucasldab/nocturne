package io.nocturne.phone.ui.browser

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Search
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.NavigationBar
import androidx.compose.material3.NavigationBarItem
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
import androidx.compose.ui.text.withStyle
import androidx.navigation.navArgument
import io.nocturne.phone.data.AppContainer
import io.nocturne.phone.player.PlayerVMFactory
import io.nocturne.phone.player.PlayerViewModel
import io.nocturne.phone.ui.search.SearchOverlay

/**
 * Top-level browser surface. Hosts the four-tab NavigationBar plus detail
 * destinations. AppRoot mounts this once it has confirmed metaTreeUri is
 * persisted AND the DB has at least one track.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun BrowserRoot(container: AppContainer) {
    val vm: BrowserViewModel = viewModel(factory = BrowserVMFactory(container))
    val playerVm: PlayerViewModel = viewModel(factory = PlayerVMFactory(container))
    DisposableEffect(playerVm) {
        playerVm.connect()
        onDispose { playerVm.disconnect() }
    }
    val nav = rememberNavController()
    var showSearch by remember { mutableStateOf(false) }

    Box(modifier = Modifier.fillMaxSize()) {
        Scaffold(
            topBar = {
                TopAppBar(
                    title = { BrandWordmark() },
                    actions = {
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
            },
                bottomBar = {
                NavigationBar(containerColor = MaterialTheme.colorScheme.surface) {
                    val current by nav.currentBackStackEntryAsState()
                    val currentRoute = current?.destination?.route
                    listOf(
                        Routes.ALBUMS to "Albums",
                        Routes.ARTISTS to "Artists",
                        Routes.TRACKS to "Tracks",
                        Routes.GENRES to "Genres",
                        Routes.SETTINGS to "Settings",
                    ).forEach { (route, label) ->
                        NavigationBarItem(
                            selected = currentRoute == route,
                            onClick = {
                                nav.navigate(route) {
                                    popUpTo(nav.graph.findStartDestination().id) {
                                        saveState = true
                                    }
                                    launchSingleTop = true
                                    restoreState = true
                                }
                            },
                            icon = {
                                Text(
                                    text = label.first().toString(),
                                    style = MaterialTheme.typography.labelMedium,
                                )
                            },
                            label = {
                                Text(text = label, style = MaterialTheme.typography.labelMedium)
                            },
                        )
                    }
                }
            },
        ) { padding ->
            NavHost(
                navController = nav,
                startDestination = Routes.ALBUMS,
                modifier = Modifier.padding(padding),
            ) {
                composable(Routes.ALBUMS) {
                    AlbumsScreen(vm, onNavigate = { id -> nav.navigate(Routes.albumDetail(id)) })
                }
                composable(Routes.ARTISTS) {
                    ArtistsScreen(vm, onNavigate = { id -> nav.navigate(Routes.artistDetail(id)) })
                }
                composable(Routes.TRACKS) {
                    TracksScreen(
                        vm = vm,
                        onTrackTap = { track ->
                            playerVm.playSingleTrack(track)
                            nav.navigate(Routes.NOW_PLAYING)
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
                            playerVm.playSingleTrack(track)
                            nav.navigate(Routes.NOW_PLAYING)
                        },
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
            )
        }
        // MiniPlayer: persistent footer above NavigationBar when a MediaItem is loaded.
        // Plain `if` -- no AnimatedVisibility (UI-SPEC Animation Gate).
        // padding(bottom = 80.dp) offsets above Material3 NavigationBar (~80dp tall).
        //
        // currentMediaItem isn't a Compose State — reading it once at composition
        // time means the mini-player never appears if a track started playing
        // while the user was already on a browse screen. We track an explicit
        // hasItem state and update it from a Player.Listener so the row pops in
        // as soon as Media3 transitions to a real item.
        val miniController = playerVm.controller.collectAsStateWithLifecycle().value
        if (miniController != null) {
            var hasItem by remember(miniController) {
                mutableStateOf(miniController.currentMediaItem != null)
            }
            DisposableEffect(miniController) {
                val listener = object : androidx.media3.common.Player.Listener {
                    override fun onMediaItemTransition(
                        mediaItem: androidx.media3.common.MediaItem?,
                        reason: Int,
                    ) {
                        hasItem = mediaItem != null
                    }
                    override fun onTimelineChanged(
                        timeline: androidx.media3.common.Timeline,
                        reason: Int,
                    ) {
                        hasItem = miniController.currentMediaItem != null
                    }
                }
                miniController.addListener(listener)
                onDispose { miniController.removeListener(listener) }
            }
            if (hasItem) {
                MiniPlayer(
                    controller = miniController,
                    onTap = { nav.navigate(Routes.NOW_PLAYING) },
                    modifier = Modifier
                        .align(Alignment.BottomCenter)
                        .padding(bottom = 80.dp),
                )
            }
        }
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
    val mutedColor = MaterialTheme.colorScheme.onSurfaceVariant
    val nameColor = MaterialTheme.colorScheme.onSurface
    val cursorColor = MaterialTheme.colorScheme.primary
    Text(
        text = buildAnnotatedString {
            withStyle(SpanStyle(color = mutedColor)) { append("$ ") }
            withStyle(SpanStyle(color = nameColor)) { append("nocturne") }
            withStyle(SpanStyle(color = cursorColor)) { append("▌") }
        },
        style = MaterialTheme.typography.titleMedium.copy(fontFamily = FontFamily.Monospace),
    )
}
