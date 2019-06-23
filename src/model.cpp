#include "model.h"

#include <glm/gtx/hash.hpp>
#include <glm/gtx/norm.hpp>
#include <glm/gtx/string_cast.hpp>
#include <iostream>
#include <unordered_map>

#include "util.h"

Model::Model(const std::vector<Triangle> &triangles) :
    m_Index(3)
{
    // default parameters
    const float pct = 0.01;

    m_LinkRestLength = 1;
    m_RadiusOfInfluence = 3;

    m_SpringFactor = pct * 1;
    m_PlanarFactor = pct * 0.5;
    m_BulgeFactor = pct * 0.1;
    m_RepulsionFactor = pct * 1;

    // find unique vertices
    std::unordered_map<glm::vec3, int> indexes;
    std::unordered_map<glm::vec3, glm::vec3> normals;
    for (const auto &t : triangles) {
        const glm::vec3 normal = t.Normal();
        for (const auto &v : {t.A(), t.B(), t.C()}) {
            if (indexes.find(v) == indexes.end()) {
                normals[v] = normal;
                indexes[v] = m_Positions.size();
                m_Positions.push_back(v);
            } else {
                normals[v] += normal;
            }
        }
    }

    // create normals
    m_Normals.resize(m_Positions.size());
    for (const auto &el : indexes) {
        m_Normals[el.second] = glm::normalize(normals[el.first]);
    }

    // create food
    m_Food.resize(m_Positions.size());

    // create links
    m_Links.resize(m_Positions.size());
    for (const auto &t : triangles) {
        const int a = indexes[t.A()];
        const int b = indexes[t.B()];
        const int c = indexes[t.C()];
        m_Links[a].push_back(b);
        m_Links[a].push_back(c);
        m_Links[b].push_back(a);
        m_Links[b].push_back(c);
        m_Links[c].push_back(a);
        m_Links[c].push_back(b);
    }

    // make links unique
    for (int i = 0; i < m_Links.size(); i++) {
        auto &links = m_Links[i];
        std::sort(links.begin(), links.end());
        links.resize(std::distance(
            links.begin(), std::unique(links.begin(), links.end())));
    }

    // build index
    for (int i = 0; i < m_Positions.size(); i++) {
        m_Index.Add(m_Positions[i], i);
    }
}

void Model::UpdateBatch(
    const int wi, const int wn,
    std::vector<glm::vec3> &newPositions,
    std::vector<glm::vec3> &newNormals) const
{
    std::vector<glm::vec3> linkedCells;
    std::vector<int> nearby;

    for (int i = wi; i < m_Positions.size(); i += wn) {
        // get linked cells
        linkedCells.resize(0);
        for (const int j : m_Links[i]) {
            linkedCells.push_back(m_Positions[j]);
        }

        // get cell position
        const glm::vec3 P = m_Positions[i];

        // update normal
        linkedCells.push_back(P);
        const glm::vec3 N = PlaneNormalFromPoints(linkedCells, m_Normals[i]);
        linkedCells.pop_back();
        newNormals[i] = N;

        // accumulate
        glm::vec3 springTarget(0);
        glm::vec3 planarTarget(0);
        float bulgeDistance = 0;
        for (const glm::vec3 &L : linkedCells) {
            springTarget += L + glm::normalize(P - L) * m_LinkRestLength;
            planarTarget += L;
            const glm::vec3 D = L - P;
            if (m_LinkRestLength > glm::length(D)) {
                const float dot = glm::dot(D, N);
                bulgeDistance += std::sqrt(
                    m_LinkRestLength * m_LinkRestLength -
                    glm::dot(D, D) + dot * dot) + dot;
            }
        }

        // average
        const float m = 1 / static_cast<float>(linkedCells.size());
        springTarget *= m;
        planarTarget *= m;
        bulgeDistance *= m;

        // repulsion
        glm::vec3 repulsionVector(0);
        const float roi2 = m_RadiusOfInfluence * m_RadiusOfInfluence;
        nearby.resize(0);
        m_Index.Search(P, m_RadiusOfInfluence, nearby);
        const auto &links = m_Links[i];
        for (const int j : nearby) {
            if (j == i) {
                continue;
            }
            if (std::find(links.begin(), links.end(), j) != links.end()) {
                continue;
            }
            const glm::vec3 D = P - m_Positions[j];
            const float d2 = glm::length2(D);
            if (d2 >= roi2) {
                continue;
            }
            const float d = (roi2 - d2) / roi2;
            repulsionVector += glm::normalize(D) * d;
        }

        // new position
        // newPositions[i] = P +
        const glm::vec3 target = P +
            m_SpringFactor * (springTarget - P) +
            m_PlanarFactor * (planarTarget - P) +
            (m_BulgeFactor * bulgeDistance) * N +
            m_RepulsionFactor * repulsionVector;

        // const glm::vec3 V = target - P;
        const float maxStep = 0.5;
        if (glm::distance(target, P) < maxStep) {
            newPositions[i] = target;
        } else {
            newPositions[i] = P + glm::normalize(target - P) * maxStep;
        }
    }
}

void Model::UpdateWithThreadPool(ctpl::thread_pool &tp) {
    std::vector<glm::vec3> newPositions;
    std::vector<glm::vec3> newNormals;
    newPositions.resize(m_Positions.size());
    newNormals.resize(m_Normals.size());

    const int wn = tp.size();
    std::vector<std::future<void>> results;
    results.resize(wn);
    for (int wi = 0; wi < wn; wi++) {
        results[wi] = tp.push([this, wi, wn, &newPositions, &newNormals](int) {
            UpdateBatch(wi, wn, newPositions, newNormals);
        });
    }
    for (int wi = 0; wi < wn; wi++) {
        results[wi].get();
    }

    // update index
    for (int i = 0; i < m_Positions.size(); i++) {
        m_Index.Update(m_Positions[i], newPositions[i], i);
    }

    // update positions and normals
    m_Positions = newPositions;
    m_Normals = newNormals;

    UpdateFood();
}

void Model::Update() {
    std::vector<glm::vec3> newPositions;
    std::vector<glm::vec3> newNormals;
    newPositions.resize(m_Positions.size());
    newNormals.resize(m_Normals.size());

    UpdateBatch(0, 1, newPositions, newNormals);

    // update index
    for (int i = 0; i < m_Positions.size(); i++) {
        m_Index.Update(m_Positions[i], newPositions[i], i);
    }

    // update positions and normals
    m_Positions = newPositions;
    m_Normals = newNormals;

    UpdateFood();
}

void Model::UpdateFood() {
    for (int i = 0; i < m_Food.size(); i++) {
        m_Food[i] += Random(0, 1);
        if (m_Food[i] > 1000) {
            Split(i);
        }
    }
}

bool Model::Linked(const int i, const int j) const {
    return std::find(
        m_Links[i].begin(), m_Links[i].end(), j) != m_Links[i].end();
}

std::vector<int> Model::OrderedLinks(const int parentIndex) const {
    std::vector<int> result = m_Links[parentIndex];
    for (int i = 1; i < result.size(); i++) {
        for (int j = i; j < result.size(); j++) {
            // is result[j] linked to result[i-1] and parent?
            if (Linked(result[i-1], result[j])) {
                std::swap(result[i], result[j]);
                break;
            }
        }
    }
    const int n = RandomIntN(result.size()-1);
    std::rotate(result.begin(), result.begin() + n, result.end());
    return result;
}

void Model::Split(const int parentIndex) {
    // get parent position
    const glm::vec3 P = m_Positions[parentIndex];

    // create the child in the same spot as the parent for now
    const int childIndex = m_Links.size();
    m_Positions.push_back(m_Positions[parentIndex]);
    m_Normals.push_back(m_Normals[parentIndex]);
    m_Food.push_back(0);
    m_Links.emplace_back();

    // All the links to one side of the plane of cleavage are left connected to
    // the parent cell, while the links to the other side are disconnected from
    // the parent and replaced with links to the daughter cell.
    std::vector<int> orderedLinks = OrderedLinks(parentIndex);
    const int n = orderedLinks.size() / 2;
    for (int i = 1; i < n; i++) {
        const int j = orderedLinks[i];
        Unlink(parentIndex, j);
        Link(childIndex, j);
    }

    // Along the plane of cleavage links are made to both the parent and
    // daughter cells. A new link is also created directly between the parent
    // and daughter.
    Link(childIndex, parentIndex);
    Link(childIndex, orderedLinks[0]);
    Link(childIndex, orderedLinks[n]);

    // compute new positions for parent and child
    glm::vec3 D0(m_Positions[parentIndex]);
    glm::vec3 D1(m_Positions[childIndex]);
    for (const int j : m_Links[parentIndex]) {
        D0 += m_Positions[j];
    }
    D0 /= m_Links[parentIndex].size() + 1;
    for (const int j : m_Links[childIndex]) {
        D1 += m_Positions[j];
    }
    D1 /= m_Links[childIndex].size() + 1;

    m_Positions[parentIndex] = D0;
    m_Positions[childIndex] = D1;
    // m_Positions[parentIndex] += N * m_LinkRestLength * 0.1f;
    // m_Positions[childIndex] -= N * m_LinkRestLength * 0.1f;

    // reset parent's food level
    m_Food[parentIndex] = 0;

    // update index
    m_Index.Update(P, m_Positions[parentIndex], parentIndex);
    m_Index.Add(m_Positions[childIndex], childIndex);
}

void Model::Link(const int i0, const int i1) {
    m_Links[i0].push_back(i1);
    m_Links[i1].push_back(i0);
}

void Model::Unlink(const int i0, const int i1) {
    auto &links0 = m_Links[i0];
    auto &links1 = m_Links[i1];
    const auto it0 = std::find(links0.begin(), links0.end(), i1);
    const auto it1 = std::find(links1.begin(), links1.end(), i0);
    std::swap(*it0, links0.back());
    std::swap(*it1, links1.back());
    links0.pop_back();
    links1.pop_back();
}

std::vector<Triangle> Model::Triangulate() const {
    std::vector<Triangle> triangles;
    for (int i = 0; i < m_Positions.size(); i++) {
        for (const int j : m_Links[i]) {
            if (j <= i) {
                continue;
            }
            for (const int k : m_Links[i]) {
                if (k <= i) {
                    continue;
                }
                if (!Linked(j, k)) {
                    continue;
                }
                const Triangle t0(m_Positions[i], m_Positions[j], m_Positions[k]);
                const Triangle t1(m_Positions[k], m_Positions[j], m_Positions[i]);
                triangles.push_back(t0);
                triangles.push_back(t1);
                // if (glm::dot(m_Normals[i], t0.Normal()) > 0) {
                //     triangles.push_back(t0);
                // } else {
                //     triangles.push_back(t1);
                // }
            }
        }
    }
    return triangles;
}
