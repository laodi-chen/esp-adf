/* Audio processor example code

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "audio_pipeline.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*tone_play_callback_t)(audio_element_status_t evt);

/**
 * @brief Enumeration of player pipeline states
 */
typedef enum {
    PIPE_STATE_IDLE,     /**< The pipeline is idle and not processing any audio */
    PIPE_STATE_RUNNING,  /**< The pipeline is actively processing and playing audio */
} pipe_player_state_e;

/**
 * @brief  A handle to the recorder pipeline structure
 */
typedef struct recorder_pipeline_t *recorder_pipeline_handle_t;

/**
 * @brief  A handle to the player pipeline structure
 */
typedef struct player_pipeline_t *player_pipeline_handle_t;

/**
 * @brief  Opens and initializes the recorder pipeline
 *
 * This function sets up the necessary resources and configurations for the recorder pipeline
 *
 * @return 
 *       - A handle to the initialized recorder pipeline
 *       - NULL if the operation fails
 */
recorder_pipeline_handle_t recorder_pipeline_open(void);

/**
 * @brief  Starts the recorder pipeline
 *
 * This function begins recording audio using the specified pipeline. It must be called after `recorder_pipeline_open()
 *
 * @param[in]  recorder_pipeline  The handle to the recorder pipeline
 * 
 * @return
 *      - ESP_OK   if the operation is successful
 *      - ESP_FAIL if the operation fails
 */
esp_err_t recorder_pipeline_run(recorder_pipeline_handle_t recorder_pipeline);

/**
 * @brief  Stops the recorder pipeline.
 *
 * This function halts the audio recording and ensures that all resources used by the pipeline are properly released
 *
 * @param[in]  recorder_pipeline  The handle to the recorder pipeline
 *
 * @return
 *      - ESP_OK   if the operation is successful
 *      - ESP_FAIL if the operation fails
 */
esp_err_t recorder_pipeline_stop(recorder_pipeline_handle_t recorder_pipeline);

/**
 * @brief Closes the recorder pipeline and releases resources.
 *
 * This function cleans up all resources associated with the recorder pipeline. It should be called after `recorder_pipeline_stop()
 *
 * @param[in]  recorder_pipeline  The handle to the recorder pipeline
 *
 * @return
 *      - ESP_OK   if the operation is successful
 *      - ESP_FAIL if the operation fails
 */
esp_err_t recorder_pipeline_close(recorder_pipeline_handle_t recorder_pipeline);

/**
 * @brief  Gets the default buffer size for reading data from the recorder pipeline
 *
 * This function returns the recommended buffer size for optimal data reading from the recorder pipeline
 *
 * @param[in]  recorder_pipeline  The handle to the recorder pipeline
 *
 * @return
 *       - > 0                 The specific length of data being read
 *       - <= 0                Error code
 */
int recorder_pipeline_get_default_read_size(recorder_pipeline_handle_t recorder_pipeline);

/**
 * @brief  Reads audio data from the recorder pipeline
 *
 * @param[in]  recorder_pipeline  The handle to the recorder pipeline.
 * @param[in]  buffer             The buffer to store the audio data.
 * @param[in]  buf_size           The size of the buffer in bytes.
 *
 * @return
 *       - > 0                 The specific length of data being read
 *       - <= 0                Error code
 */
int recorder_pipeline_read(recorder_pipeline_handle_t recorder_pipeline, char *buffer, int buf_size);

/**
 * @brief Opens and initializes the player pipeline
 *
 * @return A handle to the initialized player pipeline, or NULL if the operation fails
 */
player_pipeline_handle_t player_pipeline_open(void);

/**
 * @brief  Starts the player pipeline
 *
 * @param[in]  player_pipeline  The handle to the player pipeline
 *
 * @return
 *      - ESP_OK   if the operation is successful
 *      - ESP_FAIL if the operation fails
 */
esp_err_t player_pipeline_run(player_pipeline_handle_t player_pipeline);

/**
 * @brief Closes the player pipeline and releases resources
 *
 * @param[in]  player_pipeline  The handle to the player pipeline
 *
 * @return
 *      - ESP_OK   if the operation is successful
 *      - ESP_FAIL if the operation fails
 */
esp_err_t player_pipeline_close(player_pipeline_handle_t player_pipeline);

/**
 * @brief  Stop the player pipeline
 *
 * @param[in]  player_pipeline   handle to the player pipeline instance
 *
 * @return
 *      - ESP_OK   if the operation is successful
 *      - ESP_FAIL if the operation fails
 */
esp_err_t player_pipeline_stop(player_pipeline_handle_t player_pipeline);

/**
 * @brief  Get the current state of the player pipeline
 *
 * @param[in]   player_pipeline  The handle to the player pipeline instance
 * @param[out]  state            A pointer to a variable where the pipeline state will be stored
 *                               The state will be set to one of the values defined in the 
 *                               `pipe_player_state_e` enumeration
 * @return
 *      - ESP_OK   if the operation is successful
 *      - ESP_FAIL if the operation fails
 */
esp_err_t player_pipeline_get_state(player_pipeline_handle_t player_pipeline, pipe_player_state_e *state);

/**
 * @brief Gets the default buffer size for writing data to the player pipeline
 *
 * @param[in]  player_pipeline  The handle to the player pipeline
 *
 * @return 
 *       - The default buffer size in bytes
 */
int player_pipeline_get_default_read_size(player_pipeline_handle_t player_pipeline);

/**
 * @brief Writes audio data to the player pipeline
 *
 * This function sends audio data to the pipeline for playback
 *
 * @param[in]  player_pipeline  The handle to the player pipeline
 * @param[in]  buffer           The buffer containing the audio data to be written
 * @param[in]  buf_size         The size of the audio data in bytes
 *
 * @return 
 *        - >= 0  The number of bytes successfully written
 *        - < 0   If an error occurs
 */
int player_pipeline_write(player_pipeline_handle_t player_pipeline, char *buffer, int buf_size);

/**
 * @brief Gets the raw write element handle from the player pipeline
 *
 * @param[in]  player_pipeline  The handle to the player pipeline
 *
 * @return 
        - The raw write element handle
        -  NULL, if the operation fails
 */
audio_element_handle_t player_pipeline_get_raw_write(player_pipeline_handle_t player_pipeline);

/**
 * @brief Initializes the audio recording engine with the specified pipeline and callback
 *
 * @param[in]  pipeline  The handle to the recorder pipeline to be used by the recording engine
 * @param[in]  cb        The callback function to handle recording events
 *
 * @return A pointer to the initialized audio recording engine instance, or NULL if the initialization fails.
 *         The returned pointer must be managed appropriately, including cleanup when no longer needed.
 */
void *audio_record_engine_init(recorder_pipeline_handle_t pipeline, rec_event_cb_t cb);

/**
 * @brief  Deinitialize the audio recording engine
 *
 * @param[in]  pipeline  The handle to the recorder pipeline to be used by the recording engine
 *
 * @return
 *       - None
 */
void audio_record_engine_deinit(recorder_pipeline_handle_t pipeline);

/**
 * @brief Initialize the audio tone.
 *
 * @return
 *      - ESP_OK    if the operation is successful
 *      - ESP_FAIL  if the operation fails
 */
esp_err_t audio_tone_init(tone_play_callback_t callbaclk);

/**
 * @brief  Deinitialize the audio tone
 *
 * @return
 *      - None
 */
void audio_tone_deinit(void);

/**
 * @brief  Play audio tone from a specified URI
 *
 * @param[in]   uri  The URI of the audio source to play. This can be a file path
 *
 * @return
 *      - ESP_OK    if the operation is successful
 *      - ESP_FAIL  if the operation fails
 */
esp_err_t audio_tone_play(const char *uri);

/**
 * @brief Stop audio tone playback
 *
 * @return
 *      - ESP_OK    if the operation is successful
 *      - ESP_FAIL  if the operation fails
 */
esp_err_t audio_tone_stop(void);

#ifdef __cplusplus
}
#endif
