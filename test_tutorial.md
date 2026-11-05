Run an SHM displaying command in terminal which will display in Godot!

To choose your own program, put its launch command after --command and its Wayland application ID after --app-id:

python3 tests/test_godot_editor.py \
  --command 'your-program arguments' \
  --app-id your.application.id

For example, run top inside foot:

python3 tests/test_godot_editor.py \
  --command 'foot --app-id=my-shm-test top' \
  --app-id my-shm-test \
  --fps 10

The app ID identifies the window; it is separate from its title or process PID. If you don’t know it, launch with WAYLAND_DEBUG=client inside --command, then search the printed artifact directory’s dwl.log for set_app_id.
