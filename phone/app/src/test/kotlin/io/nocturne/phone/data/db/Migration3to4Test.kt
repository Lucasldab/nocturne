package io.nocturne.phone.data.db

import androidx.room.Room
import androidx.room.testing.MigrationTestHelper
import androidx.test.core.app.ApplicationProvider
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import io.nocturne.phone.data.db.entity.LikeEntity
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.runBlocking
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.annotation.Config

@RunWith(AndroidJUnit4::class)
@Config(sdk = [33])
class Migration3to4Test {

    private val DB_NAME = "migration-3-4-test.db"

    @get:Rule
    val helper = MigrationTestHelper(
        InstrumentationRegistry.getInstrumentation(),
        NocturneDatabase::class.java,
    )

    @Test
    fun migrate3to4_addsPinnedColumnAndLikesTable_preservesExistingPin() {
        // Open at v=3 with the auto-generated 3.json schema, seed one pin.
        helper.createDatabase(DB_NAME, 3).use { db ->
            db.execSQL(
                """INSERT INTO pins (id, unit, pinnedAt, synced)
                   VALUES ('t-pre', 'track', 1700000000000, 0)""",
            )
        }
        helper.runMigrationsAndValidate(DB_NAME, 4, true, MIGRATION_3_4).use { db ->
            // Pin row survived and now has pinned = 1 (the default).
            db.query("SELECT pinned, synced FROM pins WHERE id = 't-pre'").use { c ->
                check(c.moveToFirst()) { "pin row 't-pre' lost during migration" }
                check(c.getInt(0) == 1) {
                    "expected pinned=1 (default) for migrated pin, got ${c.getInt(0)}"
                }
                check(c.getInt(1) == 0) { "expected synced=0 (preserved) for migrated pin" }
            }
            // Likes table exists and is empty.
            db.query("SELECT name FROM sqlite_master WHERE type='table' AND name='likes'").use { c ->
                check(c.moveToFirst()) { "likes table missing after migration" }
            }
            db.query("SELECT COUNT(*) FROM likes").use { c ->
                c.moveToFirst()
                check(c.getInt(0) == 0) { "expected likes table to be empty post-migration" }
            }
        }
    }

    @Test
    fun freshV4Db_isUsableAndLikesEmpty() = runBlocking {
        val ctx = ApplicationProvider.getApplicationContext<android.content.Context>()
        val db = Room.inMemoryDatabaseBuilder(ctx, NocturneDatabase::class.java)
            .addMigrations(MIGRATION_2_3, MIGRATION_3_4)
            .build()
        try {
            check(db.likeDao().count() == 0) { "fresh v=4 DB should have empty likes table" }
            // Insert a like, then read back via Flow.
            db.likeDao().upsert(
                LikeEntity(id = "t1", unit = "track", liked = true, likedAt = 1L),
            )
            check(db.likeDao().isLiked("t1", "track").first() == true)
        } finally {
            db.close()
        }
    }
}
