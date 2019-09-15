#include "scene.h"
#include <algorithm>
#include <iostream>
#include <map>
#include <numeric>
#include <stdexcept>
#include <vector>
#include "buffer_view.h"
#include "flatten_gltf.h"
#include "gltf_types.h"
#include "stb_image.h"
#include "tiny_gltf.h"
#include "tiny_obj_loader.h"
#include "util.h"
#include <glm/ext.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>

#ifdef PBRT_PARSER_ENABLED
#include "pbrtParser/Scene.h"
#endif

struct VertIdxLess {
    bool operator()(const glm::uvec3 &a, const glm::uvec3 &b) const
    {
        return a.x < b.x || (a.x == b.x && a.y < b.y) || (a.x == b.x && a.y == b.y && a.z < b.z);
    }
};

bool operator==(const glm::uvec3 &a, const glm::uvec3 &b)
{
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

Scene::Scene(const std::string &fname)
{
    const std::string ext = get_file_extension(fname);
    if (ext == "obj") {
        load_obj(fname);
    } else if (ext == "gltf" || ext == "glb") {
        load_gltf(fname);
#ifdef PBRT_PARSER_ENABLED
    } else if (ext == "pbrt" || ext == "pbf") {
        load_pbrt(fname);
#endif
    } else {
        std::cout << "Unsupported file type '" << ext << "'\n";
        throw std::runtime_error("Unsupported file type " + ext);
    }
}

size_t Scene::unique_tris() const
{
    return std::accumulate(meshes.begin(), meshes.end(), 0, [](const size_t &n, const Mesh &m) {
        return n + m.num_tris();
    });
}

size_t Scene::total_tris() const
{
    return std::accumulate(
        instances.begin(), instances.end(), 0, [&](const size_t &n, const Instance &i) {
            return n + meshes[i.mesh_id].num_tris();
        });
}

size_t Scene::num_geometries() const
{
    return std::accumulate(meshes.begin(), meshes.end(), 0, [](const size_t &n, const Mesh &m) {
        return n + m.geometries.size();
    });
}

void Scene::load_obj(const std::string &file)
{
    std::cout << "Loading OBJ: " << file << "\n";

    std::vector<uint32_t> material_ids;
    // Load the model w/ tinyobjloader. We just take any OBJ groups etc. stuff
    // that may be in the file and dump them all into a single OBJ model.
    tinyobj::attrib_t attrib;
    std::vector<tinyobj::shape_t> shapes;
    std::vector<tinyobj::material_t> obj_materials;
    std::string err, warn;
    const std::string obj_base_dir = file.substr(0, file.rfind('/'));
    bool ret = tinyobj::LoadObj(
        &attrib, &shapes, &obj_materials, &warn, &err, file.c_str(), obj_base_dir.c_str());
    if (!warn.empty()) {
        std::cout << "TinyOBJ loading '" << file << "': " << warn << "\n";
    }
    if (!ret || !err.empty()) {
        throw std::runtime_error("TinyOBJ Error loading " + file + " error: " + err);
    }

    Mesh mesh;
    for (size_t s = 0; s < shapes.size(); ++s) {
        // We load with triangulate on so we know the mesh will be all triangle faces
        const tinyobj::mesh_t &obj_mesh = shapes[s].mesh;

        // We've got to remap from 3 indices per-vert (independent for pos, normal & uv) used by
        // tinyobjloader over to single index per-vert (single for pos, normal & uv tuple) used by
        // renderers
        std::map<glm::uvec3, uint32_t, VertIdxLess> index_mapping;
        Geometry geom;
        // Note: not supporting per-primitive materials
        geom.material_id = obj_mesh.material_ids[0];

        auto minmax_matid =
            std::minmax_element(obj_mesh.material_ids.begin(), obj_mesh.material_ids.end());
        if (*minmax_matid.first != *minmax_matid.second) {
            std::cout
                << "Warning: per-face material IDs are not supported, materials may look wrong."
                   " Please reexport your mesh with each material group as an OBJ group\n";
        }

        for (size_t f = 0; f < obj_mesh.num_face_vertices.size(); ++f) {
            if (obj_mesh.num_face_vertices[f] != 3) {
                throw std::runtime_error("Non-triangle face found in " + file + "-" +
                                         shapes[s].name);
            }

            glm::uvec3 tri_indices;
            for (size_t i = 0; i < 3; ++i) {
                const glm::uvec3 idx(obj_mesh.indices[f * 3 + i].vertex_index,
                                     obj_mesh.indices[f * 3 + i].normal_index,
                                     obj_mesh.indices[f * 3 + i].texcoord_index);
                uint32_t vert_idx = 0;
                auto fnd = index_mapping.find(idx);
                if (fnd != index_mapping.end()) {
                    vert_idx = fnd->second;
                } else {
                    vert_idx = geom.vertices.size();
                    index_mapping[idx] = vert_idx;

                    geom.vertices.emplace_back(attrib.vertices[3 * idx.x],
                                               attrib.vertices[3 * idx.x + 1],
                                               attrib.vertices[3 * idx.x + 2]);

                    if (idx.y != -1) {
                        glm::vec3 n(attrib.normals[3 * idx.y],
                                    attrib.normals[3 * idx.y + 1],
                                    attrib.normals[3 * idx.y + 2]);
                        geom.normals.push_back(glm::normalize(n));
                    }

                    if (idx.z != -1) {
                        geom.uvs.emplace_back(attrib.texcoords[2 * idx.z],
                                              attrib.texcoords[2 * idx.z + 1]);
                    }
                }
                tri_indices[i] = vert_idx;
            }
            geom.indices.push_back(tri_indices);
        }
        mesh.geometries.push_back(geom);
    }
    meshes.push_back(mesh);

    // OBJ has a single "instance"
    instances.emplace_back(glm::mat4(1.f), 0);

    std::unordered_map<std::string, int32_t> texture_ids;
    // Parse the materials over to a similar DisneyMaterial representation
    for (const auto &m : obj_materials) {
        DisneyMaterial d;
        d.base_color = glm::vec3(m.diffuse[0], m.diffuse[1], m.diffuse[2]);
        d.specular = glm::clamp(m.shininess / 500.f, 0.f, 1.f);
        d.roughness = 1.f - d.specular;
        d.specular_transmission = glm::clamp(1.f - m.dissolve, 0.f, 1.f);

        if (!m.diffuse_texname.empty()) {
            std::string path = m.diffuse_texname;
            canonicalize_path(path);
            if (texture_ids.find(m.diffuse_texname) == texture_ids.end()) {
                texture_ids[m.diffuse_texname] = textures.size();
                textures.emplace_back(obj_base_dir + "/" + path, m.diffuse_texname, SRGB);
            }
            d.color_tex_id = texture_ids[m.diffuse_texname];
        }
        materials.push_back(d);
    }

    const bool need_default_mat =
        std::find_if(meshes.begin(), meshes.end(), [](const Mesh &m) {
            return std::find_if(m.geometries.begin(), m.geometries.end(), [](const Geometry &g) {
                       return g.material_id == uint32_t(-1);
                   }) != m.geometries.end();
        }) != meshes.end();

    if (need_default_mat) {
        std::cout
            << "No materials assigned for some or all objects, generating a default material\n";
        const uint32_t default_mat_id = materials.size();
        materials.push_back(DisneyMaterial());

        // OBJ will have just one mesh, containg all geometries
        for (auto &g : meshes[0].geometries) {
            if (g.material_id == uint32_t(-1)) {
                g.material_id = default_mat_id;
            }
        }
    }

    // OBJ will not have any lights in it, so just generate one
    std::cout << "Generating light for OBJ scene\n";
    QuadLight light;
    light.emission = glm::vec4(5.f);
    light.normal = glm::vec4(glm::normalize(glm::vec3(0.5, -0.8, -0.5)), 0);
    light.position = -10.f * light.normal;
    ortho_basis(light.v_x, light.v_y, glm::vec3(light.normal));
    light.width = 5.f;
    light.height = 5.f;
    lights.push_back(light);
}

void Scene::load_gltf(const std::string &fname)
{
    std::cout << "Loading GLTF " << fname << "\n";

    tinygltf::Model model;
    tinygltf::TinyGLTF context;
    std::string err, warn;
    bool ret = false;
    if (get_file_extension(fname) == "gltf") {
        ret = context.LoadASCIIFromFile(&model, &err, &warn, fname.c_str());
    } else {
        ret = context.LoadBinaryFromFile(&model, &err, &warn, fname.c_str());
    }

    if (!warn.empty()) {
        std::cout << "TinyGLTF loading: " << fname << " warnings: " << warn << "\n";
    }

    if (!ret || !err.empty()) {
        throw std::runtime_error("TinyGLTF Error loading " + fname + " error: " + err);
    }

    flatten_gltf(model);

    std::cout << "Default scene: " << model.defaultScene << "\n";

    std::cout << "# of scenes " << model.scenes.size() << "\n";
    for (const auto &scene : model.scenes) {
        std::cout << "Scene: " << scene.name << "\n";
    }

    // Load the meshes
    for (auto &m : model.meshes) {
        Mesh mesh;
        for (auto &p : m.primitives) {
            Geometry geom;
            geom.material_id = p.material;

            if (p.mode != TINYGLTF_MODE_TRIANGLES) {
                std::cout << "Unsupported primitive mode! File must contain only triangles\n";
                throw std::runtime_error(
                    "Unsupported primitive mode! Only triangles are supported");
            }

            // Note: assumes there is a POSITION (is this required by the gltf spec?)
            Accessor<glm::vec3> pos_accessor(model.accessors[p.attributes["POSITION"]], model);
            for (size_t i = 0; i < pos_accessor.size(); ++i) {
                geom.vertices.push_back(pos_accessor[i]);
            }

            // Note: GLTF can have multiple texture coordinates used by different textures (owch)
            // I don't plan to support this
            auto fnd = p.attributes.find("TEXCOORD_0");
            if (fnd != p.attributes.end()) {
                Accessor<glm::vec2> uv_accessor(model.accessors[fnd->second], model);
                for (size_t i = 0; i < uv_accessor.size(); ++i) {
                    geom.uvs.push_back(uv_accessor[i]);
                }
            }

#if 0
            fnd = p.attributes.find("NORMAL");
            if (fnd != p.attributes.end()) {
                Accessor<glm::vec3> normal_accessor(model.accessors[fnd->second], model);
                for (size_t i = 0; i < normal_accessor.size(); ++i) {
                    geom.normals.push_back(normal_accessor[i]);
                }
            }
#endif

            if (model.accessors[p.indices].componentType ==
                TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT) {
                Accessor<uint16_t> index_accessor(model.accessors[p.indices], model);
                for (size_t i = 0; i < index_accessor.size() / 3; ++i) {
                    geom.indices.push_back(glm::uvec3(index_accessor[i * 3],
                                                      index_accessor[i * 3 + 1],
                                                      index_accessor[i * 3 + 2]));
                }
            } else if (model.accessors[p.indices].componentType ==
                       TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT) {
                Accessor<uint32_t> index_accessor(model.accessors[p.indices], model);
                for (size_t i = 0; i < index_accessor.size() / 3; ++i) {
                    geom.indices.push_back(glm::uvec3(index_accessor[i * 3],
                                                      index_accessor[i * 3 + 1],
                                                      index_accessor[i * 3 + 2]));
                }
            } else {
                std::cout << "Unsupported index type\n";
                throw std::runtime_error("Unsupported index component type");
            }
            mesh.geometries.push_back(geom);
        }
        meshes.push_back(mesh);
    }

    // Load images
    for (const auto &img : model.images) {
        std::cout << "Image: " << img.name << " (" << img.width << "x" << img.height << "):\n"
                  << "components: " << img.component << ", bits: " << img.bits << ", pixel type: "
                  << print_data_type(gltf_type_to_dtype(TINYGLTF_TYPE_SCALAR, img.pixel_type))
                  << " img vec size: " << img.image.size() << "\n";
        if (img.component != 4) {
            std::cout << "WILL: Check non-4 component image support\n";
        }
        if (img.pixel_type != TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE) {
            std::cout << "Non-uchar images are not supported\n";
            throw std::runtime_error("Unsupported image pixel type");
        }

        Image texture;
        texture.name = img.name;
        texture.width = img.width;
        texture.height = img.height;
        texture.channels = img.component;
        texture.img = img.image;
        // Assume linear unless we find it used as a color texture
        texture.color_space = LINEAR;
        textures.push_back(texture);
    }

    // Load materials
    for (const auto &m : model.materials) {
        DisneyMaterial mat;
        mat.base_color.x = m.pbrMetallicRoughness.baseColorFactor[0];
        mat.base_color.y = m.pbrMetallicRoughness.baseColorFactor[1];
        mat.base_color.z = m.pbrMetallicRoughness.baseColorFactor[2];

        mat.metallic = m.pbrMetallicRoughness.metallicFactor;

        mat.roughness = m.pbrMetallicRoughness.roughnessFactor;

        if (m.pbrMetallicRoughness.baseColorTexture.index != -1) {
            mat.color_tex_id = model.textures[m.pbrMetallicRoughness.baseColorTexture.index].source;
            // If the texture is used as a color texture we know it must be srgb space
            textures[mat.color_tex_id].color_space = SRGB;
        }

        std::cout << "mat: " << m.name << "\n"
                  << "base color: " << glm::to_string(mat.base_color) << "\n"
                  << "metallic: " << mat.metallic << "\n"
                  << "roughness: " << mat.roughness << "\n"
                  << "color texture: " << mat.color_tex_id << "\n";

        materials.push_back(mat);
    }

    for (const auto &nid : model.scenes[model.defaultScene].nodes) {
        const tinygltf::Node &n = model.nodes[nid];
        std::cout << "node: " << n.name << "\n";
        std::cout << "mesh: " << n.mesh << "\n";
        if (n.mesh != -1) {
            const glm::mat4 transform = read_node_transform(n);
            std::cout << "Transform: " << glm::to_string(transform) << "\n";
            instances.emplace_back(transform, n.mesh);
        }
    }

    // Does GLTF have lights in the file? If one is missing we should generate one,
    // otherwise we can load them
    std::cout << "Generating light for GLTF scene\n";
    QuadLight light;
    light.emission = glm::vec4(5.f);
    light.normal = glm::vec4(glm::normalize(glm::vec3(0.5, -0.8, -0.5)), 0);
    light.position = -10.f * light.normal;
    ortho_basis(light.v_x, light.v_y, glm::vec3(light.normal));
    light.width = 5.f;
    light.height = 5.f;
    lights.push_back(light);
}

#ifdef PBRT_PARSER_ENABLED

void Scene::load_pbrt(const std::string &file)
{
    std::shared_ptr<pbrt::Scene> scene = nullptr;
    try {
        if (get_file_extension(file) == "pbrt") {
            scene = pbrt::importPBRT(file);
        } else {
            scene = pbrt::Scene::loadFrom(file);
        }

        if (!scene) {
            throw std::runtime_error("Failed to load PBRT scene from " + file);
        }

        scene->makeSingleLevel();
    } catch (const std::runtime_error &e) {
        std::cout << "Error loading PBRT scene " << file << "\n";
        throw e;
    }

    // TODO: The world can also have some top-level shapes we may need to load
    for (const auto &obj : scene->world->shapes) {
        if (obj->material) {
            std::cout << "Mat: " << obj->material->toString() << "\n";
        }

        if (obj->areaLight) {
            std::cout << "Encountered area light\n";
        }

        if (pbrt::TriangleMesh::SP mesh = std::dynamic_pointer_cast<pbrt::TriangleMesh>(obj)) {
            std::cout << "Found root level triangle mesh w/ " << mesh->index.size()
                      << " triangles: " << mesh->toString() << "\n";
        } else if (pbrt::QuadMesh::SP mesh = std::dynamic_pointer_cast<pbrt::QuadMesh>(obj)) {
            std::cout << "Encountered root level quadmesh (unsupported type). Will TODO maybe "
                         "triangulate\n";
        } else {
            std::cout << "un-handled root level geometry type : " << obj->toString() << std::endl;
        }
    }

    // For PBRTv3 Each Mesh corresponds to a PBRT Object, consisting of potentially multiple Shapes.
    // This maps to a Mesh with multiple geometries, which can then be instanced
    // TODO: Maybe use https://github.com/greg7mdp/parallel-hashmap for perf.
    std::unordered_map<std::string, size_t> pbrt_objects;
    for (const auto &inst : scene->world->instances) {
        // Note: Materials are per-shape, so we should parse them and the IDs when loading the
        // shapes

        // Check if this object has already been loaded for the instance
        auto fnd = pbrt_objects.find(inst->object->name);
        size_t mesh_id = -1;
        if (fnd == pbrt_objects.end()) {
            std::cout << "Loading newly encountered instanced object " << inst->object->name
                      << "\n";

            std::vector<Geometry> geometries;
            for (const auto &g : inst->object->shapes) {
                if (pbrt::TriangleMesh::SP mesh =
                        std::dynamic_pointer_cast<pbrt::TriangleMesh>(g)) {
                    std::cout << "Object triangle mesh w/ " << mesh->index.size()
                              << " triangles: " << mesh->toString() << "\n";

                    Geometry geom;
                    geom.vertices.reserve(mesh->vertex.size());
                    std::transform(mesh->vertex.begin(),
                                   mesh->vertex.end(),
                                   std::back_inserter(geom.vertices),
                                   [](const pbrt::vec3f &v) { return glm::vec3(v.x, v.y, v.z); });

                    geom.indices.reserve(mesh->index.size());
                    std::transform(mesh->index.begin(),
                                   mesh->index.end(),
                                   std::back_inserter(geom.indices),
                                   [](const pbrt::vec3i &v) { return glm::ivec3(v.x, v.y, v.z); });

                    geom.uvs.reserve(mesh->texcoord.size());
                    std::transform(mesh->texcoord.begin(),
                                   mesh->texcoord.end(),
                                   std::back_inserter(geom.uvs),
                                   [](const pbrt::vec2f &v) { return glm::vec2(v.x, v.y); });

                    geometries.push_back(geom);
                } else if (pbrt::QuadMesh::SP mesh = std::dynamic_pointer_cast<pbrt::QuadMesh>(g)) {
                    std::cout
                        << "Encountered instanced quadmesh (unsupported type). Will TODO maybe "
                           "triangulate\n";
                } else {
                    std::cout << "un-handled instanced geometry type : " << g->toString()
                              << std::endl;
                }
            }
            if (inst->object->instances.size() > 0) {
                std::cout << "Warning: Potentially multilevel instancing is in the scene after "
                             "flattening?\n";
            }
            // Mesh only contains unsupported objects, skip it
            if (geometries.empty()) {
                std::cout << "WARNING: Instance contains only unsupported geometries, skipping\n";
                continue;
            }
            mesh_id = meshes.size();
            pbrt_objects[inst->object->name] = meshes.size();
            meshes.emplace_back(geometries);
        } else {
            mesh_id = fnd->second;
        }

        glm::mat4 transform(1.f);
        transform[0] = glm::vec4(inst->xfm.l.vx.x, inst->xfm.l.vx.y, inst->xfm.l.vx.z, 0.f);
        transform[1] = glm::vec4(inst->xfm.l.vy.x, inst->xfm.l.vy.y, inst->xfm.l.vy.z, 0.f);
        transform[2] = glm::vec4(inst->xfm.l.vz.x, inst->xfm.l.vz.y, inst->xfm.l.vz.z, 0.f);
        transform[3] = glm::vec4(inst->xfm.p.x, inst->xfm.p.y, inst->xfm.p.z, 1.f);

        instances.emplace_back(transform, mesh_id);
    }

    const uint32_t default_mat_id = materials.size();
    materials.push_back(DisneyMaterial());
    for (auto &m : meshes) {
        for (auto &g : m.geometries) {
            if (g.material_id == uint32_t(-1)) {
                g.material_id = default_mat_id;
            }
        }
    }

    std::cout << "Generating light for PBRT scene, TODO Will: Load them from the file\n";
    QuadLight light;
    light.emission = glm::vec4(5.f);
    light.normal = glm::vec4(glm::normalize(glm::vec3(0.5, -0.8, -0.5)), 0);
    light.position = -10.f * light.normal;
    ortho_basis(light.v_x, light.v_y, glm::vec3(light.normal));
    light.width = 5.f;
    light.height = 5.f;
    lights.push_back(light);
}

#endif
