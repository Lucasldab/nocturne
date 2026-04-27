package io.nocturne.phone.data.db

import androidx.room.Room
import androidx.room.testing.MigrationTestHelper
import androidx.test.core.app.ApplicationProvider
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.runBlocking
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.annotation.Config

@RunWith(AndroidJUnit4::class)
@Config(sdk = [33])
class MigrationTest {

    private val DB_NAME = "migration-test.db"

    @get:Rule
    val helper = MigrationTestHelper(
        InstrumentationRegistry.getInstrumentation(),
        NocturneDatabase::class.java,
    )

    @Test
    fun migrate2to3CreatesPinsTableAndPreservesSeededAlbums() {
        // Open at v=2 with the auto-generated 2.json schema, seed one album row.
        helper.createDatabase(DB_NAME, 2).use { db ->
            db.execSQL(
                """INSERT INTO albums (id, title, albumArtist, albumArtistId,
                   year, trackCount, totalSizeBytes, hasResident)
                   VALUES ('a1','Album One','["Artist"]','aa1',2024,1,1000,0)""",
            )
        }
        // Run migration to v=3 with strict validation.
        helper.runMigrationsAndValidate(DB_NAME, 3, true, MIGRATION_2_3).use { db ->
            // pins table now exists; album survives.
            db.query("SELECT name FROM sqlite_master WHERE type='table' AND name='pins'").use { c ->
                check(c.moveToFirst()) { "pins table missing after migration" }
            }
            db.query("SELECT id FROM albums WHERE id='a1'").use { c ->
                check(c.moveToFirst()) { "seeded album row lost during migration" }
            }
        }
    }

    @Test
    fun freshV3DbIsUsableAndPinsTableEmpty() = runBlocking {
        val ctx = ApplicationProvider.getApplicationContext<android.content.Context>()
        val db = Room.inMemoryDatabaseBuilder(ctx, NocturneDatabase::class.java)
            .addMigrations(MIGRATION_2_3)
            .build()
        try {
            check(db.pinDao().count() == 0) { "fresh v=3 DB should have empty pins table" }
            check(db.pinDao().allPinnedIds().first().isEmpty())
        } finally {
            db.close()
        }
    }
}
