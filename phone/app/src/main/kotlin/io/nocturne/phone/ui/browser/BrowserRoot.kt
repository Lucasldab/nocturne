package io.nocturne.phone.ui.browser

import androidx.compose.foundation.layout.padding
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.NavigationBar
import androidx.compose.material3.NavigationBarItem
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.Modifier
import androidx.lifecycle.viewmodel.compose.viewModel
import androidx.navigation.NavGraph.Companion.findStartDestination
import androidx.navigation.NavType
import androidx.navigation.compose.NavHost
import androidx.navigation.compose.composable
import androidx.navigation.compose.currentBackStackEntryAsState
import androidx.navigation.compose.rememberNavController
import androidx.navigation.navArgument
import io.nocturne.phone.data.AppContainer

/**
 * Top-level browser surface. Hosts the four-tab NavigationBar plus detail
 * destinations. AppRoot mounts this once it has confirmed metaTreeUri is
 * persisted AND the DB has at least one track.
 */
@Composable
fun BrowserRoot(container: AppContainer) {
    val vm: BrowserViewModel = viewModel(factory = BrowserVMFactory(container))
    val nav = rememberNavController()

    Scaffold(
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
                                popUpTo(nav.graph.findStartDestination().id) { saveState = true }
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
                AlbumDetailScreen(albumId = id, vm = vm, onBack = { nav.popBackStack() })
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
        }
    }
}
