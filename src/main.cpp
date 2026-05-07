// Copyright (c) 2020 Guy Turcotte
//
// MIT License. Look at file licenses.txt for details.

#define __GLOBAL__ 1
#include "global.hpp"

#if EPUB_INKPLATE_BUILD
  // InkPlate6 main function and main task
  
  #include "freertos/FreeRTOS.h"
  #include "freertos/task.h"

  #include "controllers/books_dir_controller.hpp"
  #include "controllers/app_controller.hpp"
  #include "models/fonts.hpp"
  #include "models/epub.hpp"
  #include "models/config.hpp"
  #include "models/nvs_mgr.hpp"
  #include "models/session_state.hpp"
  #include "models/wake_snapshot.hpp"
  #include "screen.hpp"
  #include "inkplate_platform.hpp"
  #if defined(BOARD_TYPE_PAPER_S3)
    #include "battery_paper_s3.hpp"
  #endif
  #include "helpers/unzip.hpp"
  #include "viewers/msg_viewer.hpp"
  #include "pugixml.hpp"
  #include "alloc.hpp"
  #include "esp.hpp"
  #include "esp_task_wdt.h"

  #if INKPLATE_6PLUS
    #include "controllers/back_lit.hpp"
  #endif

  #include <stdio.h>

  #if TESTING
    #include "gtest/gtest.h"
  #endif

  static constexpr char const * TAG = "main";

  void
  mainTask(void * params)
  {
    LOG_I("EPub-Inkplate Startup.");

    bool nvs_mgr_res = nvs_mgr.setup();

    // Read the deep-sleep marker very early so all later boot-path code
    // can branch on SessionState::is_warm_wake(). The marker is consumed
    // (cleared) on read so any crash mid-boot defaults the next attempt
    // to a safe cold-boot path.
    SessionState::init_at_boot();

    #if DEBUGGING
      for (int i = 10; i > 0; i--) {
        printf("\r%02d ...", i);
        fflush(stdout);
        vTaskDelay(1000 / portTICK_PERIOD_MS);
      }
      printf("\n"); fflush(stdout);
    #endif

    #if TESTING

      testing::InitGoogleTest();
      RUN_ALL_TESTS();
      while (1) {
        printf(".");
        vTaskDelay(10000 / portTICK_PERIOD_MS);
      }
    
    #else
      bool inkplate_err = !inkplate_platform.setup(true);
      if (inkplate_err) LOG_E("InkPlate6Ctrl Error.");

      #if defined(BOARD_TYPE_PAPER_S3)
        if (!battery.setup()) {
          LOG_E("Battery monitor setup failed; level reads will return 0.");
        }
      #endif

      bool config_err = !config.read();

      #if DATE_TIME_RTC
        if (config_err) {
          LOG_E("Config Error.");
        }
        else {
          std::string time_zone;

          config.get(Config::Ident::TIME_ZONE, time_zone);
          setenv("TZ", time_zone.c_str(), 1);
        }
      #else
        if (config_err) {
          LOG_E("Config Error.");
        }
      #endif
      
      #if DEBUGGING
        config.show();
      #endif

      pugi::set_memory_management_functions(allocate, free);

      page_locs.setup();

      #if INKPLATE_6PLUS
        #define MSG "Press the WakUp Button to restart."
        #define INT_PIN TouchScreen::INTERRUPT_PIN
        #define LEVEL 0
      #else
        #define MSG "Press a key to restart."
        #if EXTENDED_CASE
          #define INT_PIN PressKeys::INTERRUPT_PIN
        #else
          #define INT_PIN TouchKeys::INTERRUPT_PIN
        #endif
        #define LEVEL 1
      #endif

      #if defined(BOARD_TYPE_PAPER_S3)
        #undef INT_PIN
        #define INT_PIN ((gpio_num_t)0)
      #endif

      if (fonts.setup()) {

        Screen::Orientation    orientation;
        Screen::PixelResolution resolution;
        config.get(Config::Ident::ORIENTATION,      (int8_t *) &orientation);

        #if defined(BOARD_TYPE_PAPER_S3)
          // Paper S3 always renders in 4-bit grayscale via epdiy. Do not
          // depend on the stored PIXEL_RESOLUTION setting here.
          resolution = Screen::PixelResolution::THREE_BITS;
        #else
          config.get(Config::Ident::PIXEL_RESOLUTION, (int8_t *) &resolution);
        #endif

        // On warm wake the panel still latches the sleep-screen image
        // from before deep sleep — keep it visible by skipping the
        // ~700 ms boot epd_fullclear.
        screen.setup(resolution, orientation,
                     /*preserve_panel_image=*/SessionState::is_warm_wake());

        // Warm-wake fast path: paint the cached snapshot (the book
        // page the user was last viewing) onto the panel before the
        // remaining boot work runs. The synchronous boot path that
        // follows — fonts.setup, page_locs.setup, books_dir scan,
        // app_controller.start - takes another 2-4 s; without this
        // the user stares at a (clearing) wallpaper that whole time.
        // With this they see their book essentially immediately, can
        // read while the rest of boot completes, and only experience
        // a brief input-dead window. event_mgr.setup() (below)
        // initializes the touch driver, which starts pushing TAP/
        // SWIPE events into input_event_queue immediately — but
        // AppController isn't ready to dispatch them yet, so they
        // pile up. AppController::start coalesces the queued batch
        // down to the single most-recent event before entering the
        // regular event loop, so a user who swiped 3× during boot
        // (each retry thinking the prior didn't register) gets the
        // ONE page turn they actually wanted, not three.
        //
        // restore_to_panel arms screen.update internally which fires
        // the s_force_full + s_warm_wake_clear_pending fullclear+GC16
        // path, so the panel cleanly transitions wallpaper -> page.
        if (SessionState::is_warm_wake()) {
          // Stage 2: capture the three out-params (book_id_out,
          // page_id_out, format_hash_out) and stash them somewhere
          // BookController::open_book_file can read so it can call
          // wake_snapshot.invalidate() if the about-to-open book
          // doesn't match the painted snapshot. Right now we paint
          // unconditionally and rely on the BookParamController
          // invalidate hooks to catch format-edit staleness; a
          // user who navigates to a different book between sleep
          // and wake will see ~2-4 s of stale content before the
          // real render replaces it.
          if (wake_snapshot.restore_to_panel(nullptr, nullptr, nullptr)) {
            LOG_I("warm-wake: snapshot painted, continuing boot");
          }
          else {
            LOG_I("warm-wake: no snapshot available; falling back to "
                  "normal boot rendering");
          }
        }

        event_mgr.setup();
        event_mgr.set_orientation(orientation);

        #if defined(BOARD_TYPE_PAPER_S3)
          // The epdiy rendering threads ("epd_prep") can fully occupy both
          // CPU cores during long grayscale updates (GC16/GL16/fullclear).
          // The ESP-IDF task watchdog by default monitors the IDLE tasks on
          // each core, so IDLE0/IDLE1 will not run and cannot feed the
          // watchdog while epd_prep is active, causing repeated task_wdt
          // warnings and potential resets even though the system is behaving
          // as expected.
          //
          // For this dedicated reader, we rely on the interrupt watchdog for
          // hard lockup protection and avoid using the Task WDT while epdiy
          // keeps the CPUs busy during display updates.
        #endif

        #if INKPLATE_6PLUS
          back_lit.setup();
        #endif

        if (!nvs_mgr_res) {
          msg_viewer.show_alert_fatal( "Hardware Problem!",
            "Failed to initialise NVS Flash. Entering Deep Sleep. " MSG
          );

          ESP::delay(500);
          inkplate_platform.deep_sleep(INT_PIN, LEVEL);
        }
    
        if (inkplate_err) {
          msg_viewer.show_alert_fatal( "Hardware Problem!",
            "Unable to initialize the InkPlate drivers. Entering Deep Sleep. " MSG
          );
          ESP::delay(500);
          inkplate_platform.deep_sleep(INT_PIN, LEVEL);
        }

        if (config_err) {
          msg_viewer.show_alert_fatal( "Configuration Problem!",
            "Unable to read/save configuration file. Entering Deep Sleep. " MSG
          );
          ESP::delay(500);
          inkplate_platform.deep_sleep(INT_PIN, LEVEL);
        }

        // On warm wake the sleep-screen / wallpaper is still latched on
        // the panel and serves as the boot splash — show "Starting…"
        // only on a real cold boot. Saves a ~600-800 ms GC16 update
        // and avoids replacing the user's wallpaper with a generic
        // placeholder for no reason.
        if (!SessionState::is_warm_wake()) {
          msg_viewer.show_info_fullscreen( "Starting", "One moment please...");
        }

        books_dir_controller.setup();
        LOG_D("Initialization completed");
        app_controller.start();
      }
      else {
        LOG_E("Font loading error.");
        msg_viewer.show_alert_fatal( "Font Loading Problem!",
          "Unable to read required fonts. Entering Deep Sleep. " MSG
        );
        ESP::delay(500);
        inkplate_platform.deep_sleep(INT_PIN, LEVEL);
      }

      #if DEBUGGING
        while (1) {
          printf("Allo!\n");
          vTaskDelay(10000 / portTICK_PERIOD_MS);
        }
      #endif
    #endif
  }

  #define STACK_SIZE 60000

  extern "C" {
    
    void 
    app_main(void)
    {
      //printf("EPub InkPlate Reader Startup\n");

      #if !defined(BOARD_TYPE_PAPER_S3)
        /* Print chip information */
        esp_chip_info_t chip_info;
        esp_chip_info(&chip_info);
        printf("This is %s chip with %d CPU core(s), WiFi%s%s, ",
                CONFIG_IDF_TARGET,
                chip_info.cores,
                (chip_info.features & CHIP_FEATURE_BT) ? "/BT" : "",
                (chip_info.features & CHIP_FEATURE_BLE) ? "/BLE" : "");

        printf("silicon revision %d, ", chip_info.revision);

        printf("%dMB %s flash\n", spi_flash_get_chip_size() / (1024 * 1024),
                (chip_info.features & CHIP_FEATURE_EMB_FLASH) ? "embedded" : "external");

        printf("Minimum free heap size: %d bytes\n", esp_get_minimum_free_heap_size());

        heap_caps_print_heap_info(MALLOC_CAP_32BIT|MALLOC_CAP_8BIT|MALLOC_CAP_SPIRAM|MALLOC_CAP_INTERNAL);
      #endif

      #if defined(BOARD_TYPE_PAPER_S3) && CONFIG_ESP_TASK_WDT_INIT
        // Disable the Task Watchdog Timer service on Paper S3. The epdiy
        // rendering threads can legitimately keep both cores busy for long
        // periods during grayscale updates, which conflicts with TWDT's
        // expectation that monitored tasks (especially idle tasks) run
        // frequently. We keep the interrupt watchdog enabled for hard
        // lockup detection and turn off TWDT to avoid spurious errors.
        esp_task_wdt_deinit();
      #endif

      TaskHandle_t xHandle = NULL;

      xTaskCreate(mainTask, "mainTask", STACK_SIZE, (void *) 1, configMAX_PRIORITIES - 1, &xHandle);
      configASSERT(xHandle);
    }

  } // extern "C"

#else

  // Linux main function

  #include "controllers/books_dir_controller.hpp"
  #include "controllers/app_controller.hpp"
  #include "viewers/msg_viewer.hpp"
  #include "models/fonts.hpp"
  #include "models/config.hpp"
  #include "models/page_locs.hpp"
  #include "screen.hpp"

  #if TESTING
    #include "gtest/gtest.h"
  #endif

  static const char * TAG = "Main";

  void exit_app()
  {
    fonts.clear_glyph_caches();
    fonts.clear(true);
    epub.close_file();
    DOM::delete_pool();
  }

  int 
  main(int argc, char **argv) 
  {
    bool config_err = !config.read();
    if (config_err) LOG_E("Config Error.");

    #if DEBUGGING
      config.show();
    #endif

    page_locs.setup();
    
    if (fonts.setup()) {

      Screen::Orientation     orientation;
      Screen::PixelResolution resolution;
      config.get(Config::Ident::ORIENTATION,     (int8_t *) &orientation);
      config.get(Config::Ident::PIXEL_RESOLUTION, (int8_t *) &resolution);
      screen.setup(resolution, orientation);

      event_mgr.setup();
      books_dir_controller.setup();

      #if defined(INKPLATE_6PLUS)
        #define MSG "the WakeUp button"
      #else
        #define MSG "a key"
      #endif

      if (config_err) {
        msg_viewer.show_alert_fatal( "Configuration Problem!",
          "Unable to read/save configuration file. Entering Deep Sleep. Press " MSG " to restart."
        );
        sleep(10);
        exit(0);
      }

      // exit(0)  // Used for some Valgrind tests
      #if TESTING
        testing::InitGoogleTest();
        return RUN_ALL_TESTS();
      #else
        app_controller.start();
      #endif
    }
    else {
      msg_viewer.show_alert_fatal( "Font Loading Problem!",
        "Unable to load default fonts."
      );

      sleep(30);
      return 1;

    }
    
    return 0;
  }

#endif