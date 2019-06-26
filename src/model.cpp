#include "model.h"

#include <glm/gtx/hash.hpp>
#include <glm/gtx/norm.hpp>
#include <glm/gtx/normal.hpp>
#include <iostream>
#include <unordered_map>

#include "util.h"

Model::Model(const std::vector<Triangle> &triangles) :
    m_Index(1)
{
    const float averageEdgeLength = [&triangles]() {
        float sum = 0;
        for (const auto &t : triangles) {
            sum += glm::distance(t.A(), t.B());
            sum += glm::distance(t.B(), t.C());
            sum += glm::distance(t.C(), t.A());
        }
        return sum / (triangles.size() * 3);
    }();

    m_LinkRestLength = averageEdgeLength * Random(0.5, 2);
    // m_LinkRestLength = 1;
    m_SplitThreshold = 100;

    m_Index = Index(m_LinkRestLength * 2);

    float pct = 0.1;
    m_RadiusOfInfluence = 2;
    m_RepulsionFactor = pct * 0.5;
    m_SpringFactor = pct * 0.5;
    m_PlanarFactor = pct * 0.5;
    m_BulgeFactor = pct * 0.5;

    pct = Random(0.01, 0.3);
    m_RadiusOfInfluence = Random(m_LinkRestLength * 1, m_LinkRestLength * 2);
    m_RepulsionFactor = pct * Random(0, 1);
    m_SpringFactor = pct * Random(0, 1);
    m_PlanarFactor = pct * Random(0, 1);
    m_BulgeFactor = pct * Random(0, 1);

    m_LinkRestLength = 0.991549;
    m_RadiusOfInfluence = 1.2939;
    m_SpringFactor = 0.188446;
    m_PlanarFactor = 0.276574;
    m_BulgeFactor = 0.139144;
    m_RepulsionFactor = 0.0938309;


    std::cout << "m_LinkRestLength = " << m_LinkRestLength << std::endl;
    std::cout << "m_RadiusOfInfluence = " << m_RadiusOfInfluence << std::endl;
    std::cout << "m_SpringFactor = " << m_SpringFactor << std::endl;
    std::cout << "m_PlanarFactor = " << m_PlanarFactor << std::endl;
    std::cout << "m_BulgeFactor = " << m_BulgeFactor << std::endl;
    std::cout << "m_RepulsionFactor = " << m_RepulsionFactor << std::endl;
    std::cout << std::endl;

    // find unique vertices
    std::unordered_map<glm::vec3, int> indexes;
    std::unordered_map<glm::vec3, std::vector<int>> vertexTriangles;
    for (int i = 0; i < triangles.size(); i++) {
        const Triangle &t = triangles[i];
        for (const auto &v : {t.A(), t.B(), t.C()}) {
            vertexTriangles[v].push_back(i);
            if (indexes.find(v) == indexes.end()) {
                indexes[v] = m_Positions.size();
                // create new cell
                m_Positions.push_back(v);
                m_Normals.emplace_back(0);
                m_Food.push_back(0);
                m_Links.emplace_back();
            }
        }
    }

    // sort triangles into CCW order for each vertex and create links
    for (auto &it : vertexTriangles) {
        const auto &point = it.first;
        auto &tris = it.second;
        for (int i0 = 1; i0 < tris.size(); i0++) {
            const glm::vec3 &prev = triangles[tris[i0-1]].VertexBefore(point);
            for (int i1 = i0; i1 < tris.size(); i1++) {
                if (triangles[tris[i1]].VertexAfter(point) == prev) {
                    std::swap(tris[i0], tris[i1]);
                    break;
                }
            }
        }
        const int i = indexes[point];
        for (const int j : tris) {
            const int k = indexes[triangles[j].VertexAfter(point)];
            m_Links[i].push_back(k);
        }
    }

    // build index
    for (int i = 0; i < m_Positions.size(); i++) {
        m_Index.Add(m_Positions[i], i);
        m_Normals[i] = CellNormal(i);
    }
}

void Model::UpdateBatch(
    const int wi, const int wn,
    std::vector<glm::vec3> &newPositions,
    std::vector<glm::vec3> &newNormals,
    std::vector<float> &newFood) const
{
    const float roi2 = m_RadiusOfInfluence * m_RadiusOfInfluence;
    const float link2 = m_LinkRestLength * m_LinkRestLength;

    for (int i = wi; i < m_Positions.size(); i += wn) {
        // get cell position, normal, and links
        const glm::vec3 P = m_Positions[i];
        const glm::vec3 N = CellNormal(i);
        const auto &links = m_Links[i];

        // accumulate
        glm::vec3 repulsionVector(0);
        glm::vec3 springTarget(0);
        glm::vec3 planarTarget(0);
        float bulgeDistance = 0;
        float food = 0;
        for (const int j : links) {
            const glm::vec3 &L = m_Positions[j];
            const glm::vec3 D = L - P;
            const glm::vec3 Dn = glm::normalize(D);
            springTarget += L - Dn * m_LinkRestLength;
            planarTarget += L;
            const float length2 = glm::length2(D);
            if (length2 < link2) {
                const float dot = glm::dot(D, N);
                bulgeDistance += std::sqrt(
                    link2 - glm::dot(D, D) + dot * dot) + dot;
            }
            food += m_Food[j];
            if (length2 < roi2) {
                // linked cells will be repulsed in the repulsion step below
                // so, here we add in the opposite to counteract it for
                // performance reasons
                const float m = (roi2 - length2) / roi2;
                repulsionVector += Dn * m;
            }
        }

        // average
        const float m = 1 / static_cast<float>(links.size());
        springTarget *= m;
        planarTarget *= m;
        bulgeDistance *= m;
        food *= m;

        // repulsion
        for (const int j : m_Index.Nearby(P)) {
            if (j == i) {
                continue;
            }
            const glm::vec3 &L = m_Positions[j];
            const glm::vec3 D = P - L;
            const float d2 = glm::length2(D);
            if (d2 < roi2) {
                const float m = (roi2 - d2) / roi2;
                repulsionVector += glm::normalize(D) * m;
            }
        }

        // results
        newNormals[i] = N;
        newPositions[i] = P +
            m_SpringFactor * (springTarget - P) +
            m_PlanarFactor * (planarTarget - P) +
            (m_BulgeFactor * bulgeDistance) * N +
            m_RepulsionFactor * repulsionVector;

        // newFood[i] += Random(0, 1);
        // newFood[i] += Random(0, 1) / (std::abs(P.y) + 1);
        // newFood[i] += glm::length(repulsionVector);
        // newFood[i] += Random(0, 1);
        // newFood[i] = food + std::pow(std::max(0.f, N.z), 2);
        // newFood[i] = food + N.z + 0.1;
        // newFood[i] += std::pow(N.z, 2);
        // newFood[i] = std::max(0.f, newFood[i]);
    }
}

void Model::UpdateWithThreadPool(ctpl::thread_pool &tp) {
    std::vector<glm::vec3> newPositions;
    std::vector<glm::vec3> newNormals;
    std::vector<float> newFood = m_Food;
    newPositions.resize(m_Positions.size());
    newNormals.resize(m_Normals.size());

    auto done = Timed("run workers");
    const int wn = tp.size();
    std::vector<std::future<void>> results(wn);
    for (int wi = 0; wi < wn; wi++) {
        results[wi] = tp.push([
            this, wi, wn, &newPositions, &newNormals, &newFood](int)
        {
            UpdateBatch(wi, wn, newPositions, newNormals, newFood);
        });
    }
    for (int wi = 0; wi < wn; wi++) {
        results[wi].get();
    }
    done();

    Commit(std::move(newPositions), std::move(newNormals), std::move(newFood));
}

void Model::Commit(
    const std::vector<glm::vec3> &&newPositions,
    const std::vector<glm::vec3> &&newNormals,
    const std::vector<float> &&newFood)
{
    // update index
    auto done = Timed("update index");
    for (int i = 0; i < m_Positions.size(); i++) {
        m_Index.Update(m_Positions[i], newPositions[i], i);
    }
    done();

    // update positions
    done = Timed("copy vectors");
    m_Positions = newPositions;
    m_Normals = newNormals;
    m_Food = newFood;
    done();

    // split
    done = Timed("split");
    for (int i = 0; i < m_Food.size(); i++) {
        m_Food[i] += Random(0, 1);
        if (m_Food[i] > m_SplitThreshold) {
            Split(i);
        }
    }
    done();
}

glm::vec3 Model::CellNormal(const int index) const {
    const auto &links = m_Links[index];
    const glm::vec3 p0 = m_Positions[index];
    glm::vec3 p1 = m_Positions[links.back()];
    glm::vec3 N(0);
    for (const int i : links) {
        const glm::vec3 p2 = m_Positions[i];
        N += glm::triangleNormal(p0, p1, p2);
        p1 = p2;
    }
    return glm::normalize(N);
}

void Model::ChangeLink(const int i, const int from, const int to) {
    auto &links = m_Links[i];
    const auto it = std::find(links.begin(), links.end(), from);
    // if (it == links.end()) {
    //     Panic("index not found in ChangeLink");
    //     return;
    // }
    *it = to;
}

void Model::InsertLinkBefore(const int i, const int before, const int link) {
    auto &links = m_Links[i];
    const auto it = std::find(links.begin(), links.end(), before);
    // if (it == links.end()) {
    //     Panic("index not found in InsertLinkAfter");
    //     return;
    // }
    links.insert(it, link);
}

void Model::InsertLinkAfter(const int i, const int after, const int link) {
    auto &links = m_Links[i];
    const auto it = std::find(links.begin(), links.end(), after);
    // if (it == links.end()) {
    //     Panic("index not found in InsertLinkAfter");
    //     return;
    // }
    links.insert(it + 1, link);
}

void Model::Split(const int parentIndex) {
    // create the child in the same spot as the parent for now
    const int childIndex = m_Links.size();
    m_Positions.push_back(m_Positions[parentIndex]);
    m_Normals.emplace_back(m_Normals[parentIndex]);
    m_Food.push_back(0);
    m_Links.emplace_back();

    // choose "plane of cleavage"
    const auto links = m_Links[parentIndex];
    const int n = links.size();
    const int i0 = RandomIntN(n);
    const int i1 = i0 + n / 2;

    // update parent links
    auto &parentLinks = m_Links[parentIndex];
    parentLinks.resize(0);
    for (int i = i0; i <= i1; i++) {
        parentLinks.push_back(links[i % n]);
    }
    parentLinks.push_back(childIndex);

    // update child links
    auto &childLinks = m_Links[childIndex];
    for (int i = i1; i <= i0 + n; i++) {
        childLinks.push_back(links[i % n]);
    }
    childLinks.push_back(parentIndex);

    // update neighbor links
    InsertLinkAfter(links[i0 % n], parentIndex, childIndex);
    InsertLinkBefore(links[i1 % n], parentIndex, childIndex);
    for (int i = i1 + 1; i <= i0 + n - 1; i++) {
        ChangeLink(links[i % n], parentIndex, childIndex);
    }

    // compute new parent position
    glm::vec3 newParentPosition(m_Positions[parentIndex]);
    for (const int j : parentLinks) {
        newParentPosition += m_Positions[j];
    }
    newParentPosition /= parentLinks.size() + 1;

    // compute new child position
    glm::vec3 newChildPosition(m_Positions[childIndex]);
    for (const int j : childLinks) {
        newChildPosition += m_Positions[j];
    }
    newChildPosition /= childLinks.size() + 1;

    // update positions, normals, and index
    m_Index.Update(m_Positions[parentIndex], newParentPosition, parentIndex);
    m_Index.Add(newChildPosition, childIndex);
    m_Positions[parentIndex] = newParentPosition;
    m_Positions[childIndex] = newChildPosition;
    m_Normals[parentIndex] = CellNormal(parentIndex);
    m_Normals[childIndex] = CellNormal(childIndex);

    // reset parent's food level
    m_Food[parentIndex] = 0;
}

std::vector<Triangle> Model::Triangulate() const {
    std::vector<glm::uvec3> indexes;
    TriangleIndexes(indexes);
    std::vector<Triangle> triangles;
    triangles.reserve(indexes.size());
    for (const auto &i : indexes) {
        triangles.emplace_back(
            m_Positions[i.x], m_Positions[i.y], m_Positions[i.z]);
    }
    return triangles;
}

void Model::TriangleIndexes(std::vector<glm::uvec3> &result) const {
    for (int i = 0; i < m_Positions.size(); i++) {
        const auto &links = m_Links[i];
        for (int j = 0; j < links.size(); j++) {
            const int k = (j + 1) % links.size();
            const int link0 = links[j];
            const int link1 = links[k];
            if (i < link0 && i < link1) {
                result.emplace_back(i, link0, link1);
            }
        }
    }
}

void Model::VertexAttributes(std::vector<float> &result) const {
    for (int i = 0; i < m_Positions.size(); i++) {
        const auto &p = m_Positions[i];
        const auto &n = m_Normals[i];
        const float value = m_Food[i] / m_SplitThreshold;
        // const float value = i / (float)(m_Positions.size() - 1);
        result.push_back(p.x);
        result.push_back(p.y);
        result.push_back(p.z);
        result.push_back(n.x);
        result.push_back(n.y);
        result.push_back(n.z);
        result.push_back(value);
    }
}
