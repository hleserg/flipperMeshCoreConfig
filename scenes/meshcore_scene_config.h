/*
 * The one place scenes are registered.
 *
 * ADD_SCENE(prefix, name, id) expands into the scene enum, the handler
 * prototypes and the handler tables — see meshcore_scene.h / .c. Adding a
 * scene means adding a line here plus scenes/meshcore_scene_<name>.c.
 *
 * Deliberately has no include guard: it is included several times with a
 * different ADD_SCENE definition each time.
 */
ADD_SCENE(meshcore, menu, Menu)
ADD_SCENE(meshcore, connect, Connect)
ADD_SCENE(meshcore, contacts, Contacts)
ADD_SCENE(meshcore, chat, Chat)
ADD_SCENE(meshcore, compose, Compose)
ADD_SCENE(meshcore, logger, Logger)
ADD_SCENE(meshcore, log, Log)
ADD_SCENE(meshcore, profiles, Profiles)
ADD_SCENE(meshcore, apply, Apply)
