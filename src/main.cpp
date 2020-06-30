#include "bubble_offset.h"

#include "geometrycentral/numerical/linear_algebra_utilities.h"
#include "geometrycentral/surface/edge_length_geometry.h"
#include "geometrycentral/surface/halfedge_factories.h"
#include "geometrycentral/surface/intrinsic_mollification.h"
#include "geometrycentral/surface/manifold_surface_mesh.h"
#include "geometrycentral/surface/meshio.h"
#include "geometrycentral/surface/signpost_intrinsic_triangulation.h"
#include "geometrycentral/surface/simple_polygon_mesh.h"
#include "geometrycentral/surface/surface_mesh.h"
#include "geometrycentral/surface/tufted_laplacian.h"
#include "geometrycentral/surface/vertex_position_geometry.h"

#include "polyscope/curve_network.h"
#include "polyscope/polyscope.h"
#include "polyscope/surface_mesh.h"

#include "args/args.hxx"
#include "imgui.h"

#include <sstream>

using namespace geometrycentral;
using namespace geometrycentral::surface;

// core data
std::unique_ptr<SurfaceMesh> mesh;
std::unique_ptr<VertexPositionGeometry> geometry;

// used only during visualization
std::unique_ptr<SurfaceMesh> tuftedMesh;
std::unique_ptr<ManifoldSurfaceMesh> manifoldTuftedMesh;
std::unique_ptr<VertexPositionGeometry> tuftedGeom;
std::unique_ptr<EdgeLengthGeometry> tuftedIntrinsicGeom;
std::unique_ptr<SignpostIntrinsicTriangulation> signpostTri;

// Parameters
float mollifyFactor = 0.;

// Viz Parameters
bool withGUI = true;
float bubbleScale = .2;
int subdivLevel = 3;
int pointsPerTriEdge = 10;

// This re-runs the whole tufted cover algorithm, but does extra processing to separate out vertex tangent spaces so
// that we can use signposts, trace edges, and make some visualizations.
void generateVertexSeparatedTuftedCover() {

  // Create a copy of the mesh / geometry to operate on
  tuftedMesh = mesh->copyToSurfaceMesh();
  geometry->requireVertexPositions();
  tuftedGeom.reset(new VertexPositionGeometry(*tuftedMesh, geometry->vertexPositions.reinterpretTo(*tuftedMesh)));
  tuftedGeom->requireEdgeLengths();
  EdgeData<double> tuftedEdgeLengths = tuftedGeom->edgeLengths;

  // Build the cover
  buildIntrinsicTuftedCover(*tuftedMesh, tuftedEdgeLengths, tuftedGeom.get());

  // Split the vertices
  VertexData<Vertex> origVert = tuftedMesh->separateNonmanifoldVertices();
  for (Vertex v : tuftedMesh->vertices()) {
    tuftedGeom->inputVertexPositions[v] = tuftedGeom->inputVertexPositions[origVert[v]];
  }
  tuftedGeom->refreshQuantities();

  // Make it manifold
  manifoldTuftedMesh = tuftedMesh->toManifoldMesh();
  manifoldTuftedMesh->printStatistics();
  tuftedGeom = tuftedGeom->reinterpretTo(*manifoldTuftedMesh);
  tuftedGeom->requireEdgeLengths();
  tuftedEdgeLengths = tuftedGeom->edgeLengths;

  // Mollify, if requested
  if (mollifyFactor > 0) {
    mollifyIntrinsic(*manifoldTuftedMesh, tuftedEdgeLengths, mollifyFactor);
  }

  tuftedIntrinsicGeom.reset(new EdgeLengthGeometry(*manifoldTuftedMesh, tuftedEdgeLengths));


  // Create a signpost triangulation
  signpostTri.reset(new SignpostIntrinsicTriangulation(*manifoldTuftedMesh, *tuftedIntrinsicGeom));

  // Flip to delaunay
  signpostTri->flipToDelaunay();

}

void generateVisualization() {

  // == Generate the the bubbly mesh visualization
  std::unique_ptr<SimplePolygonMesh> subSoup =
      subdivideRounded(*manifoldTuftedMesh, *tuftedGeom, subdivLevel, bubbleScale, 0, 0);
  polyscope::registerSurfaceMesh("bubble tufted cover", subSoup->vertexCoordinates, subSoup->polygons);


  // == Trace intrinsic edges across the bubbly mesh

  BubbleOffset bubbleOffset(*tuftedGeom);
  bubbleOffset.relativeScale = bubbleScale;

  // Build out the output here
  SimplePolygonMesh outSoup;
  std::vector<std::vector<Vector3>> lines;

  // Trace the halfedges as lines
  auto pushLinePoint = [&](SurfacePoint p) {
    Vector3 newPos = bubbleOffset.queryPoint(p);
    lines.back().push_back(newPos);
  };
  for (Edge e : signpostTri->mesh.edges()) {
    Halfedge he = e.halfedge();

    // This is bit of an ugly hack.
    // This length factor works around amibiguity in sharedFace() below; there could be multiple
    // shared faces, but stopping early helps create a surface point in the one we want.
    double oldLen = signpostTri->intrinsicEdgeLengths[e];
    signpostTri->intrinsicEdgeLengths[e] *= .999;

    std::vector<SurfacePoint> points = signpostTri->traceHalfedge(he, false);

    signpostTri->intrinsicEdgeLengths[e] = oldLen; // restore the pre-adjusted length from above

    lines.emplace_back();

    // = first point
    pushLinePoint(points.front());

    for (size_t i = 0; i + 1 < points.size(); i++) {

      // = interpolation between
      SurfacePoint& pA = points[i];
      SurfacePoint& pB = points[i + 1];

      // get both points in some face
      Face sharedF = sharedFace(pA, pB);
      SurfacePoint pAF = pA.inFace(sharedF);
      SurfacePoint pBF = pB.inFace(sharedF);

      for (int iInterp = 0; iInterp < pointsPerTriEdge; iInterp++) {
        double tInterp = static_cast<double>(iInterp + 1) / (pointsPerTriEdge + 1);

        Vector3 baryInterp = (1. - tInterp) * pAF.faceCoords + tInterp * pBF.faceCoords;
        SurfacePoint pInterp(sharedF, baryInterp);
        pushLinePoint(pInterp);
      }

      // = next point
      pushLinePoint(pB);
    }

    // pushLinePoint(SurfacePoint(he.twin().vertex()));
  }

  polyscope::getSurfaceMesh("bubble tufted cover")->addSurfaceGraphQuantity("intrinsic edges", lines)->setEnabled(true);
}


template <typename T>
void saveMatrix(std::string filename, SparseMatrix<T>& matrix) {

  // WARNING: this follows matlab convention and thus is 1-indexed

  std::cout << "Writing sparse matrix to: " << filename << std::endl;

  std::ofstream outFile(filename);
  if (!outFile) {
    throw std::runtime_error("failed to open output file " + filename);
  }

  // Write a comment on the first line giving the dimensions
  // outFile << "# sparse " << matrix.rows() << " " << matrix.cols() << std::endl;

  outFile << std::setprecision(16);

  for (int k = 0; k < matrix.outerSize(); ++k) {
    for (typename SparseMatrix<T>::InnerIterator it(matrix, k); it; ++it) {
      T val = it.value();
      size_t iRow = it.row();
      size_t iCol = it.col();

      outFile << (iRow + 1) << " " << (iCol + 1) << " " << val << std::endl;
    }
  }

  outFile.close();
}

void myCallback() {

  ImGui::PushItemWidth(100);

  ImGui::TextUnformatted("Intrinsic triangulation:");
  // ImGui::Text("  nVertices = %lu  nFaces = %lu", signpostTri->mesh.nVertices(), signpostTri->mesh.nFaces());

  ImGui::SetNextTreeNodeOpen(true);
  if (ImGui::TreeNode("Visualization")) {
    ImGui::InputInt("subivsion rounds", &subdivLevel);
    ImGui::SliderFloat("bubble scale", &bubbleScale, .0, .5);
    ImGui::InputInt("points per tri edge", &pointsPerTriEdge);

    if (ImGui::Button("Regenerate visualization")) {
      generateVisualization();
    }

    ImGui::TreePop();
  }

  ImGui::PopItemWidth();
}

int main(int argc, char** argv) {

  // Configure the argument parser
  // clang-format off
  args::ArgumentParser parser("Demo for ");
  args::HelpFlag help(parser, "help", "Display this help message", {'h', "help"});
  args::Positional<std::string> inputFilename(parser, "mesh", "A surface mesh file (see geometry-central for valid formats)");

  args::Group algorithmOptions(parser, "algorithm options");
  args::ValueFlag<double> mollifyFactorArg(algorithmOptions, "mollifyFactor", "Amount of intrinsic mollification to perform, which gives robustness to degenerate triangles. Defined relative to the mean edge length. Default: 1e-6", {"mollifyFactor"}, 1e-6);

  args::Group output(parser, "ouput");
  args::Flag gui(output, "gui", "open a GUI after processing and generate some visualizations", {"gui"});
  args::ValueFlag<std::string> outputPrefixArg(output, "outputPrefix", "Prefix to prepend to output file paths. Default: tufted_", {"outputPrefix"}, "tufted_");
  args::Flag writeLaplacian(output, "writeLaplacian", "Write out the resulting (weak) Laplacian as a sparse matrix. name: 'laplacian.spmat'", {"writeLaplacian"});
  args::Flag writeMass(output, "writeMass", "Write out the resulting diagonal lumped mass matrix sparse matrix. name: 'lumped_mass.spmat'", {"writeMass"});
  // clang-format on

  // Parse args
  try {
    parser.ParseCLI(argc, argv);
  } catch (args::Help) {
    std::cout << parser;
    return 0;
  } catch (args::ParseError e) {
    std::cerr << e.what() << std::endl;
    std::cerr << parser;
    return 1;
  }

  // Make sure a mesh name was given
  if (!inputFilename) {
    std::cout << parser;
    return EXIT_FAILURE;
  }

  // Set options
  withGUI = gui;
  mollifyFactor = args::get(mollifyFactorArg);
  std::string outputPrefix = args::get(outputPrefixArg);

  // Load mesh
  SimplePolygonMesh inputMesh(args::get(inputFilename));

  inputMesh.stripFacesWithDuplicateVertices();
  inputMesh.stripUnusedVertices();
  inputMesh.triangulate();

  std::tie(mesh, geometry) = makeGeneralHalfedgeAndGeometry(inputMesh.polygons, inputMesh.vertexCoordinates);

  // ta-da! (invoke the algorithm from geometry-central)
  std::cout << "Building tufted Laplacian..." << std::endl;
  SparseMatrix<double> L, M;
  std::tie(L, M) = buildTuftedLaplacian(*mesh, *geometry, mollifyFactor);
  std::cout << "  ...done!" << std::endl;

  // write output matrices, if requested
  if (writeLaplacian) {
    saveMatrix(outputPrefix + "laplacian.spmat", L);
  }
  if (writeMass) {
    saveMatrix(outputPrefix + "lumped_mass.spmat", M);
  }

  if (withGUI) {
    std::cout << "Generating visualization..." << std::endl;

    // Initialize polyscope
    polyscope::init();

    // Run the totally-separate version of the algorithm with signposts for tracing
    generateVertexSeparatedTuftedCover();
    generateVisualization();

    // Set the callback function
    polyscope::state::userCallback = myCallback;

    // The mesh
    polyscope::registerSurfaceMesh("input mesh", inputMesh.vertexCoordinates, inputMesh.polygons);

    // Give control to the polyscope gui
    std::cout << "  ...done!" << std::endl;
    polyscope::show();
  }

  return EXIT_SUCCESS;
}