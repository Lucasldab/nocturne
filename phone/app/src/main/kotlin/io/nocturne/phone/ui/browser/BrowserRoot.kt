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
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import androidx.lifecycle.viewmodel.compose.viewModel
import androidx.navigation.NavGraph.Companion.findStartDestination
import androidx.navigation.NavType
import androidx.navigation.compose.NavHost
import androidx.navigation.compose.composable
import androidx.navigation.compose.currentBackStackEntryAsState
import androidx.navigation.compose.rememberNavController
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
                    title = {
                        Text("nocturne", style = MaterialTheme.typography.titleMedium)
                    },
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
                composable(Routes.TRACKS) { TracksScreen(vm) }
                composable(Routes.GENRES) { GenresScreen(vm) }
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
                    )
                }
                composable(Routes.NOW_PLAYING) {
                    // Plan 05-03 stub — 05-05 replaces with real NowPlayingScreen
                    Box(
                        modifier = Modifier
                            .fillMaxSize()
                            .background(MaterialTheme.colorScheme.background),
                    ) {
                        Text(
                            "now playing (05-05 stub)",
                            style = MaterialTheme.typography.bodyMedium,
                            color = MaterialTheme.colorScheme.onBackground,
                            modifier = Modifier.padding(16.dp),
                        )
                    }
                }
            }
        }
        if (showSearch) {
            SearchOverlay(container = container, onDismiss = { showSearch = false })
        }
    }
}
